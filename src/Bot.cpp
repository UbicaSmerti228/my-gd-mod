#include "Bot.hpp"
#include "analysis/Analyzer.hpp"
#include "sim/SimController.hpp"
#include "analysis/LStar.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

using namespace geode::prelude;

namespace {
    constexpr char const* REPLAY_MAGIC = "ILR";
    constexpr int REPLAY_VERSION = 5;
    constexpr char const* REPLAY_EXT = ".ilr";

    bool isValidName(std::string const& name) {
        if (name.empty() || name.size() > 64) return false;
        return std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.';
        }) && name.find("..") == std::string::npos;
    }

    PlayerState playerState(PlayerObject* player) {
        if (!player) return {};
        auto pos = player->getPosition();
        return { pos.x, pos.y, player->m_yVelocity };
    }
}

Bot& Bot::get() {
    static Bot instance;
    return instance;
}

Bot::Bot() {
    speed = Mod::get()->getSavedValue<float>("speed", 1.f);
    showOverlay = Mod::get()->getSavedValue<bool>("show-overlay", true);
    showCounter = Mod::get()->getSavedValue<bool>("show-counter", true);
    showPaths = Mod::get()->getSavedValue<bool>("show-paths", true);
    playSounds = Mod::get()->getSavedValue<bool>("play-sounds", true);
    useCbf = Mod::get()->getSavedValue<bool>("use-cbf", true);
    cbfMode = Mod::get()->getSavedValue<bool>("cbf-mode", false);
}

void Bot::setMode(BotMode newMode) {
    mode = newMode;
    injecting = false;
    if (mode == BotMode::Record || mode == BotMode::Play) {
        // Click Between Frames moves the player between ticks and changes how many ticks
        // run per frame: a recording made with it does not replay, and the analysis stops.
        if (auto cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            if (cbf->getSettingValue<bool>("physics-bypass")) {
                Notification::create("Turn off Physics Bypass in Click Between Frames: the bot needs 240 ticks per second",
                    NotificationIcon::Warning, 5.f)->show();
            }
            else if (!cbfMode && mode == BotMode::Record) {
                Notification::create("Click Between Frames is on: enable CBF mode in the bot menu, or the recording will not replay",
                    NotificationIcon::Warning, 5.f)->show();
            }
        }
    }
    if (mode == BotMode::Record) {
        // A fresh recording always starts from the beginning of the next attempt.
        replay.inputs.clear();
        clearAnalysis();
        track.clear();
    }
}

void Bot::setSpeed(float value) {
    speed = std::clamp(value, 0.05f, 2.f);
    Mod::get()->setSavedValue("speed", speed);
    if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
        engine->m_globalChannel->setPitch(speed);
    }
}

void Bot::clearAnalysis() {
    replay.analysis.clear();
    replay.lstar.clear();
    replay.lstarShare.clear();
    Analyzer::get().clearPaths();
}

void Bot::onLevelStart(GJGameLevel* level) {
    tick = 0;
    playIndex = 0;
    measuredTps = 0;
    if (mode == BotMode::Record) {
        replay = Replay{};
        replay.levelID = level ? level->m_levelID.value() : 0;
        replay.levelName = level ? std::string(level->m_levelName.c_str()) : "";
        track.clear();
    }
}

void Bot::onReset(GJBaseGameLayer* layer) {
    splits.clear();
    m_lastStepX[0] = m_lastStepX[1] = 0.f;
    m_tpsStartTick = tick;
    m_tpsStartTime = layer->m_gameState.m_levelTime;

    if (mode == BotMode::Record && !analyzing) {
        // Throw away everything recorded after the point we respawned at.
        auto& in = replay.inputs;
        in.erase(
            std::remove_if(in.begin(), in.end(), [&](BotInput const& i) { return i.tick >= tick; }),
            in.end()
        );

        // Buttons still held in the recording are released, both in the replay and
        // in the game, so the next press after respawning is recorded as a real press.
        std::map<std::pair<uint8_t, bool>, bool> held;
        for (auto const& i : in) held[{ i.button, i.player1 }] = i.down;
        for (auto const& [key, isDown] : held) {
            if (!isDown) continue;
            in.push_back({ tick, key.first, key.second, false });
            injecting = true;
            layer->handleButton(false, key.first, key.second);
            injecting = false;
        }
    }
    else if (mode == BotMode::Play || analyzing) {
        auto const& in = playbackInputs();
        playIndex = std::lower_bound(
            in.begin(), in.end(), tick,
            [](BotInput const& i, uint32_t t) { return i.tick < t; }
        ) - in.begin();
        if (analyzing) {
            if (auto sim = SimController::active()) sim->onReset(layer);
        }
    }
}

void Bot::applySplitsNow(GJBaseGameLayer* layer) {
    auto pending = std::move(splits);
    splits.clear();
    for (auto const& i : pending) {
        injecting = true;
        layer->handleButton(i.down, i.button, i.player1);
        injecting = false;
    }
}

void Bot::onTickStart(GJBaseGameLayer* layer) {
    // Inputs the player update never picked up are applied on the tick boundary.
    if (!splits.empty()) this->applySplitsNow(layer);

    PlayerObject* players[2] = { layer->m_player1, layer->m_player2 };
    for (int p = 0; p < 2; ++p) {
        float x = players[p] ? players[p]->getPositionX() : 0.f;
        m_lastStepX[p] = x - m_tickStartX[p];
        m_tickStartX[p] = x;
    }

    if (mode != BotMode::Play && !analyzing) return;
    auto const& in = playbackInputs();
    while (playIndex < in.size() && in[playIndex].tick <= tick) {
        auto index = playIndex++;
        auto const& i = in[index];
        if (i.subtick > 0.f) {
            splits.push_back(i);
            if (!analyzing && onInputPlayed) onInputPlayed(index);
            continue;
        }
        injecting = true;
        layer->handleButton(i.down, i.button, i.player1);
        injecting = false;
        if (!analyzing && onInputPlayed) onInputPlayed(index);
    }
}

void Bot::onTickEnd(GJBaseGameLayer* layer) {
    if (analyzing) {
        if (auto sim = SimController::active()) {
            sim->onTickEnd(layer, tick);
            // The level is beaten the moment the end animation starts; levelComplete itself
            // only comes after the animation, in real time, possibly during a later run.
            if (layer->m_levelEndAnimationStarted) sim->onComplete();
        }
    }
    else if (mode == BotMode::Play) {
        if (track.size() <= tick) track.resize(tick + 1);
        track[tick] = captureState(layer);
    }

    ++tick;

    double now = layer->m_gameState.m_levelTime;
    double elapsed = now - m_tpsStartTime;
    if (elapsed >= 1.0) {
        measuredTps = static_cast<int>(std::lround((tick - m_tpsStartTick) / elapsed));
        m_tpsStartTick = tick;
        m_tpsStartTime = now;
        if (mode == BotMode::Record) replay.tps = measuredTps;
    }
}

void Bot::record(GJBaseGameLayer* layer, bool down, int button, bool player1) {
    uint32_t at = tick;
    float subtick = 0.f;
    // How far into this tick's step the player had moved when the click arrived. Before
    // the step (the normal case) the click belongs to this tick. After the whole step it
    // can only affect the next one. In between is a click between ticks, which only Click
    // Between Frames (or the game's Click Between Steps) produces: kept in CBF mode,
    // otherwise rounded to the nearest tick.
    if (inTick) {
        int p = !player1 && layer->m_gameState.m_isDualMode ? 1 : 0;
        auto player = p ? layer->m_player2 : layer->m_player1;
        float step = m_lastStepX[p];
        if (player && std::abs(step) > 0.01f) {
            float fraction = (player->getPositionX() - m_tickStartX[p]) / step;
            if (fraction >= 0.999f || (!cbfMode && fraction >= 0.5f)) at = tick + 1;
            else if (cbfMode && fraction > 0.001f) subtick = fraction;
        }
    }
    // Keep the list in time order (a click moved to the next tick may follow later ones).
    BotInput input { at, static_cast<uint8_t>(button), player1, down, subtick };
    auto pos = std::upper_bound(replay.inputs.begin(), replay.inputs.end(), input, [](auto const& a, auto const& b) {
        return a.tick < b.tick || (a.tick == b.tick && a.subtick < b.subtick);
    });
    replay.inputs.insert(pos, input);
    unsaved = true;
}

TickState Bot::captureState(GJBaseGameLayer* layer) {
    TickState state;
    state.p1 = playerState(layer->m_player1);
    state.p2 = playerState(layer->m_player2);
    state.dual = layer->m_gameState.m_isDualMode;
    state.valid = true;
    return state;
}

std::filesystem::path Bot::replayDir() {
    return Mod::get()->getSaveDir() / "replays";
}

std::vector<std::string> Bot::listReplays() {
    std::vector<std::string> names;
    auto files = file::readDirectory(replayDir());
    if (!files) return names;
    for (auto const& path : files.unwrap()) {
        if (path.extension() == REPLAY_EXT) names.push_back(path.stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

Result<> Bot::save(std::string const& name) const {
    if (!isValidName(name)) return Err("Name may only contain letters, digits, spaces, _ - .");
    if (replay.inputs.empty()) return Err("Nothing recorded yet");
    if (auto res = file::createDirectoryAll(replayDir()); !res) return Err(res.unwrapErr());

    std::ostringstream out;
    out << REPLAY_MAGIC << ' ' << REPLAY_VERSION << '\n';
    out << "level " << replay.levelID << '\n';
    out << "tps " << replay.tps << '\n';
    out << "inputs " << replay.inputs.size() << '\n';
    for (auto const& i : replay.inputs) {
        out << i.tick << ' ' << int(i.button) << ' ' << int(i.player1) << ' ' << int(i.down) << ' ' << i.subtick << '\n';
    }
    if (replay.hasAnalysis()) {
        out << "analysis " << replay.analysis.size() << '\n';
        for (auto const& a : replay.analysis) {
            int flags = (a.analyzed ? 1 : 0) | (a.capped ? 2 : 0) | (a.unreliable ? 4 : 0);
            out << a.left << ' ' << a.right << ' ' << flags << ' ' << a.cbf << ' ' << a.x << '\n';
        }
    }
    // The level name goes last since it may contain spaces.
    out << "name " << replay.levelName << '\n';

    auto res = file::writeStringSafe(replayDir() / (name + REPLAY_EXT), out.str());
    if (!res) return Err(res.unwrapErr());
    return Ok();
}

void Bot::markSaved(std::string const& name) {
    replayName = name;
    unsaved = false;
}

Result<Replay> Bot::readReplay(std::string const& name) {
    if (!isValidName(name)) return Err("Invalid replay name");
    auto data = file::readString(replayDir() / (name + REPLAY_EXT));
    if (!data) return Err(data.unwrapErr());

    std::istringstream in(data.unwrap());
    std::string magic, key;
    int version = 0;
    if (!(in >> magic >> version) || magic != REPLAY_MAGIC || version < 1 || version > REPLAY_VERSION) {
        return Err("Not an ILR replay");
    }

    Replay loaded;
    size_t count = 0;
    if (!(in >> key >> loaded.levelID) || key != "level") return Err("Corrupted replay (level)");
    if (!(in >> key >> loaded.tps) || key != "tps") return Err("Corrupted replay (tps)");
    if (!(in >> key >> count) || key != "inputs") return Err("Corrupted replay (inputs)");

    loaded.inputs.reserve(count);
    for (size_t n = 0; n < count; ++n) {
        uint32_t tick; int button, player1, down;
        float subtick = 0.f;
        if (!(in >> tick >> button >> player1 >> down)) return Err("Corrupted replay (input list)");
        if (version >= 5 && !(in >> subtick)) return Err("Corrupted replay (input list)");
        loaded.inputs.push_back({ tick, static_cast<uint8_t>(button), player1 != 0, down != 0, std::clamp(subtick, 0.f, 0.999f) });
    }

    while (in >> key) {
        if (key == "analysis") {
            size_t n = 0;
            if (!(in >> n) || n != loaded.inputs.size()) return Err("Corrupted replay (analysis)");
            loaded.analysis.resize(n);
            for (auto& a : loaded.analysis) {
                int flags = 0;
                if (!(in >> a.left >> a.right >> flags)) return Err("Corrupted replay (analysis list)");
                if (version >= 3 && !(in >> a.cbf)) return Err("Corrupted replay (analysis list)");
                if (version >= 4 && !(in >> a.x)) return Err("Corrupted replay (analysis list)");
                a.analyzed = flags & 1;
                a.capped = flags & 2;
                a.unreliable = flags & 4;
            }
        }
        else if (key == "name") {
            std::getline(in >> std::ws, loaded.levelName);
            break;
        }
    }

    // Inputs are written sorted; an out-of-order file cannot keep its analysis aligned.
    auto byTime = [](auto const& a, auto const& b) {
        return a.tick < b.tick || (a.tick == b.tick && a.subtick < b.subtick);
    };
    if (!std::is_sorted(loaded.inputs.begin(), loaded.inputs.end(), byTime)) {
        std::stable_sort(loaded.inputs.begin(), loaded.inputs.end(), byTime);
        loaded.analysis.clear();
    }
    return Ok(std::move(loaded));
}

Result<> Bot::load(std::string const& name) {
    auto loaded = readReplay(name);
    if (!loaded) return Err(loaded.unwrapErr());

    Analyzer::get().clearPaths();
    replay = std::move(loaded).unwrap();
    replayName = name;
    unsaved = false;
    playIndex = 0;
    track.clear();
    LStar::computeAsync();
    return Ok();
}

Result<> Bot::remove(std::string const& name) {
    if (!isValidName(name)) return Err("Invalid replay name");
    std::error_code ec;
    std::filesystem::remove(replayDir() / (name + REPLAY_EXT), ec);
    if (ec) return Err(ec.message());
    return Ok();
}
