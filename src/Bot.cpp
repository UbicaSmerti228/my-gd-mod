#include "Bot.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace geode::prelude;

namespace {
    constexpr char const* REPLAY_MAGIC = "ILR";
    constexpr int REPLAY_VERSION = 1;
    constexpr char const* REPLAY_EXT = ".ilr";

    bool isValidName(std::string const& name) {
        if (name.empty() || name.size() > 64) return false;
        return std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.';
        }) && name.find("..") == std::string::npos;
    }
}

Bot& Bot::get() {
    static Bot instance;
    return instance;
}

Bot::Bot() {
    speed = Mod::get()->getSavedValue<float>("speed", 1.f);
    showOverlay = Mod::get()->getSavedValue<bool>("show-overlay", true);
}

void Bot::setMode(BotMode newMode) {
    mode = newMode;
    injecting = false;
    if (mode == BotMode::Record) {
        // A fresh recording always starts from the beginning of the next attempt.
        replay.inputs.clear();
    }
}

void Bot::setSpeed(float value) {
    speed = std::clamp(value, 0.05f, 2.f);
    Mod::get()->setSavedValue("speed", speed);
    if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
        engine->m_globalChannel->setPitch(speed);
    }
}

void Bot::onLevelStart(GJGameLevel* level) {
    tick = 0;
    playIndex = 0;
    measuredTps = 0;
    if (mode == BotMode::Record) {
        replay = Replay{};
        replay.levelID = level ? level->m_levelID.value() : 0;
        replay.levelName = level ? std::string(level->m_levelName.c_str()) : "";
    }
}

void Bot::onReset(GJBaseGameLayer* layer) {
    m_tpsStartTick = tick;
    m_tpsStartTime = layer->m_gameState.m_levelTime;

    if (mode == BotMode::Record) {
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
    else if (mode == BotMode::Play) {
        auto& in = replay.inputs;
        playIndex = std::lower_bound(
            in.begin(), in.end(), tick,
            [](BotInput const& i, uint32_t t) { return i.tick < t; }
        ) - in.begin();
    }
}

void Bot::onTickStart(GJBaseGameLayer* layer) {
    if (mode != BotMode::Play) return;
    auto const& in = replay.inputs;
    while (playIndex < in.size() && in[playIndex].tick <= tick) {
        auto const& i = in[playIndex++];
        injecting = true;
        layer->handleButton(i.down, i.button, i.player1);
        injecting = false;
    }
}

void Bot::onTickEnd(GJBaseGameLayer* layer) {
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

void Bot::record(bool down, int button, bool player1) {
    replay.inputs.push_back({ tick, static_cast<uint8_t>(button), player1, down });
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
        out << i.tick << ' ' << int(i.button) << ' ' << int(i.player1) << ' ' << int(i.down) << '\n';
    }
    // The level name goes last since it may contain spaces.
    out << "name " << replay.levelName << '\n';

    auto res = file::writeStringSafe(replayDir() / (name + REPLAY_EXT), out.str());
    if (!res) return Err(res.unwrapErr());
    return Ok();
}

Result<> Bot::load(std::string const& name) {
    if (!isValidName(name)) return Err("Invalid replay name");
    auto data = file::readString(replayDir() / (name + REPLAY_EXT));
    if (!data) return Err(data.unwrapErr());

    std::istringstream in(data.unwrap());
    std::string magic, key;
    int version = 0;
    if (!(in >> magic >> version) || magic != REPLAY_MAGIC || version != REPLAY_VERSION) {
        return Err("Not an ILR v1 replay");
    }

    Replay loaded;
    size_t count = 0;
    if (!(in >> key >> loaded.levelID) || key != "level") return Err("Corrupted replay (level)");
    if (!(in >> key >> loaded.tps) || key != "tps") return Err("Corrupted replay (tps)");
    if (!(in >> key >> count) || key != "inputs") return Err("Corrupted replay (inputs)");

    loaded.inputs.reserve(count);
    for (size_t n = 0; n < count; ++n) {
        uint32_t tick; int button, player1, down;
        if (!(in >> tick >> button >> player1 >> down)) return Err("Corrupted replay (input list)");
        loaded.inputs.push_back({ tick, static_cast<uint8_t>(button), player1 != 0, down != 0 });
    }
    if (in >> key && key == "name") {
        std::getline(in >> std::ws, loaded.levelName);
    }

    std::stable_sort(loaded.inputs.begin(), loaded.inputs.end(), [](auto const& a, auto const& b) {
        return a.tick < b.tick;
    });
    replay = std::move(loaded);
    playIndex = 0;
    return Ok();
}

Result<> Bot::remove(std::string const& name) {
    if (!isValidName(name)) return Err("Invalid replay name");
    std::error_code ec;
    std::filesystem::remove(replayDir() / (name + REPLAY_EXT), ec);
    if (ec) return Err(ec.message());
    return Ok();
}
