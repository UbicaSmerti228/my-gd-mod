#include "Analyzer.hpp"
#include "LStar.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

using namespace geode::prelude;

namespace {
    constexpr float POSITION_EPSILON = 0.01f;
    constexpr double VELOCITY_EPSILON = 0.01;
    // Wall-clock time the analyzer may spend per rendered frame.
    constexpr auto FRAME_BUDGET = std::chrono::milliseconds(14);
    // Updates in a row without a physics tick before yielding the frame (the game may be
    // waiting on a real-time delay, e.g. the start of the level).
    constexpr int MAX_IDLE_STEPS_PER_FRAME = 50;
    // Frames in a row without any physics tick before we give up.
    constexpr int MAX_STALLED_FRAMES = 900;

    bool samePlayer(PlayerState const& a, PlayerState const& b) {
        return std::abs(a.x - b.x) <= POSITION_EPSILON
            && std::abs(a.y - b.y) <= POSITION_EPSILON
            && std::abs(a.yVelocity - b.yVelocity) <= VELOCITY_EPSILON;
    }

    bool sameState(TickState const& a, TickState const& b) {
        return samePlayer(a.p1, b.p1) && samePlayer(a.p2, b.p2);
    }

    bool sameButton(BotInput const& a, BotInput const& b) {
        return a.button == b.button && a.player1 == b.player1;
    }

    int s_stalledFrames = 0;
}

Analyzer& Analyzer::get() {
    static Analyzer instance;
    return instance;
}

float Analyzer::progress() const {
    auto count = Bot::get().replay.inputs.size();
    if (m_phase == Phase::Idle || m_phase == Phase::Starting || m_phase == Phase::Reference || count == 0) return 0.f;
    return static_cast<float>(m_input) / static_cast<float>(count);
}

std::string Analyzer::statusText() const {
    auto count = Bot::get().replay.inputs.size();
    switch (m_phase) {
        case Phase::Idle: return "";
        case Phase::Starting: return "Analysis: resume the game to start";
        case Phase::Reference: return fmt::format("Analysis: reference run, tick {}", Bot::get().tick);
        case Phase::Advance:
        case Phase::Candidate:
            return fmt::format("Analysis: {:.1f}%  input {}/{}  offset {:+d}",
                progress() * 100.f, m_input + 1, count, m_offset);
    }
    return "";
}

void Analyzer::clearPaths() {
    paths.clear();
}

Result<> Analyzer::start(PlayLayer* layer) {
    auto& bot = Bot::get();
    if (isActive()) return Err("Analysis is already running");
    if (!layer) return Err("Open the level first");
    if (bot.replay.inputs.empty()) return Err("Load or record a replay first");
    int levelID = layer->m_level ? layer->m_level->m_levelID.value() : 0;
    if (bot.replay.levelID && levelID && bot.replay.levelID != levelID) {
        return Err("This replay was recorded on another level");
    }
    if (layer->m_isPlatformer) return Err("Platformer levels are not supported");

    maxWindow = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("max-window", 20)), 13, 120);
    settleTicks = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("settle-ticks", 10)), 0, 240);

    m_wasPractice = layer->m_isPracticeMode;
    bot.mode = BotMode::Play;
    bot.analyzing = true;
    bot.simInputs = bot.replay.inputs;
    paths.clear();
    m_results.clear();
    m_phase = Phase::Starting;
    return Ok();
}

void Analyzer::cancel(PlayLayer* layer) {
    if (!isActive()) return;
    finish(layer, false, "Analysis cancelled");
}

void Analyzer::drive(PlayLayer* layer, std::function<void(float)> const& step) {
    auto& bot = Bot::get();

    if (m_phase == Phase::Starting) {
        if (!m_wasPractice) layer->togglePracticeMode(true);
        if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
            engine->m_globalChannel->getVolume(&m_oldVolume);
            engine->m_globalChannel->setVolume(0.f);
        }
        s_stalledFrames = 0;
        beginReference(layer);
    }

    auto deadline = std::chrono::steady_clock::now() + FRAME_BUDGET;
    bool advanced = false;
    int idleSteps = 0;
    while (isActive() && std::chrono::steady_clock::now() < deadline) {
        if (m_restorePending) {
            m_restorePending = false;
            this->restore(layer, m_restoreFrom);
            m_runDone = m_died = m_completed = m_converged = m_mismatch = false;
            continue;
        }

        uint32_t before = bot.tick;
        // One update must run exactly one physics tick, so the carried-over time is dropped.
        layer->m_extraDelta = 0.0;
        float warp = std::min(layer->m_gameState.m_timeWarp, 1.f);
        if (!(warp > 0.f)) warp = 1.f;
        step(warp / static_cast<float>(bot.replay.effectiveTps()));
        uint32_t after = bot.tick;

        if (after > before + 1) {
            return finish(layer, false, "The game ran several physics ticks in one update, analysis stopped");
        }
        if (after == before) {
            if (++idleSteps > MAX_IDLE_STEPS_PER_FRAME) break;
            continue;
        }
        idleSteps = 0;
        advanced = true;

        if (m_phase == Phase::Advance && !m_runDone && after == m_target) {
            m_snapshot = layer->createCheckpoint();
            m_snapshotTick = m_target;
            m_hasSnapshot = true;
            this->beginCandidate(layer, 0);
            continue;
        }

        if (!m_runDone) continue;

        switch (m_phase) {
            case Phase::Reference: {
                if (m_died) {
                    return finish(layer, false, fmt::format("The replay dies at tick {}, analysis needs a replay that passes", bot.tick));
                }
                auto count = bot.replay.inputs.size();
                m_results.assign(count, {});
                paths.assign(count, {});
                m_input = 0;
                m_snapshot = nullptr;
                m_snapshotTick = 0;
                m_hasSnapshot = true;
                this->prepareInput(layer);
                break;
            }
            case Phase::Advance:
                // Reaching the checkpoint tick is handled above; anything else means the
                // replay did not reproduce between the reference and this run.
                return finish(layer, false, fmt::format("The replay did not reproduce (died at tick {})", bot.tick));
            case Phase::Candidate:
                this->finishCandidate(layer);
                break;
            default:
                break;
        }
    }

    if (!isActive()) return;
    if (advanced || m_restorePending) {
        s_stalledFrames = 0;
    }
    else if (++s_stalledFrames > MAX_STALLED_FRAMES) {
        finish(layer, false, "The level stopped advancing, analysis stopped");
    }
}

void Analyzer::beginReference(PlayLayer* layer) {
    auto& bot = Bot::get();
    bot.simInputs = bot.replay.inputs;
    m_phase = Phase::Reference;
    m_referenceEnd = bot.replay.lastTick() + 240;
    bot.track.assign(m_referenceEnd + 1, {});
    m_restoreFrom = nullptr;
    m_restorePending = true;
}

uint32_t Analyzer::snapshotTickFor(size_t index) const {
    auto const& in = Bot::get().replay.inputs;
    uint32_t tick = in[index].tick;

    int64_t earliest = static_cast<int64_t>(tick) - maxWindow;
    for (size_t j = index; j-- > 0;) {
        if (sameButton(in[j], in[index])) {
            earliest = std::max<int64_t>(earliest, in[j].tick + 1);
            break;
        }
    }
    earliest = std::clamp<int64_t>(earliest, 0, tick);

    // Reuse the checkpoint we already have when it is early enough.
    if (m_hasSnapshot && m_snapshotTick >= earliest && m_snapshotTick <= tick) return m_snapshotTick;
    return static_cast<uint32_t>(earliest);
}

uint32_t Analyzer::endTickFor(size_t index, int offset) const {
    auto const& bot = Bot::get();
    auto const& in = bot.replay.inputs;
    uint32_t moved = static_cast<uint32_t>(static_cast<int64_t>(in[index].tick) + offset);
    uint32_t changed = std::max(in[index].tick, moved);

    uint32_t end = changed + 60;
    for (size_t k = index + 1; k < in.size(); ++k) {
        if (in[k].player1 == in[index].player1 && in[k].tick > changed) {
            end = in[k].tick;
            break;
        }
    }
    end += settleTicks;
    if (!bot.track.empty()) end = std::min<uint32_t>(end, static_cast<uint32_t>(bot.track.size() - 1));
    return std::max(end, changed);
}

bool Analyzer::offsetAllowed(int offset) const {
    if (std::abs(offset) > maxWindow) return false;
    auto const& in = Bot::get().replay.inputs;
    auto const& input = in[m_input];
    int64_t moved = static_cast<int64_t>(input.tick) + offset;
    if (moved < static_cast<int64_t>(m_snapshotTick)) return false;

    // An input may not cross the previous or next event of the same button:
    // that would turn a press into a release or merge two clicks.
    for (size_t j = m_input; j-- > 0;) {
        if (sameButton(in[j], input)) {
            if (moved <= static_cast<int64_t>(in[j].tick)) return false;
            break;
        }
    }
    for (size_t k = m_input + 1; k < in.size(); ++k) {
        if (sameButton(in[k], input)) {
            if (moved >= static_cast<int64_t>(in[k].tick)) return false;
            break;
        }
    }
    return true;
}

void Analyzer::prepareInput(PlayLayer* layer) {
    auto& bot = Bot::get();
    auto const& in = bot.replay.inputs;

    // Inputs after the end of the reference run (after the level was beaten) are skipped.
    while (m_input < in.size()) {
        auto tick = in[m_input].tick;
        if (tick < bot.track.size() && bot.track[tick].valid) break;
        ++m_input;
    }
    if (m_input >= in.size()) {
        return finish(layer, true, "Analysis finished");
    }

    uint32_t snapshot = snapshotTickFor(m_input);
    if (m_hasSnapshot && snapshot == m_snapshotTick) {
        return this->beginCandidate(layer, 0);
    }
    this->beginAdvance(layer, snapshot);
}

void Analyzer::beginAdvance(PlayLayer* layer, uint32_t target) {
    auto& bot = Bot::get();
    if (target == 0) {
        m_snapshot = nullptr;
        m_snapshotTick = 0;
        m_hasSnapshot = true;
        return this->beginCandidate(layer, 0);
    }

    bot.simInputs = bot.replay.inputs;
    bool canContinue = m_hasSnapshot && m_snapshotTick <= target;
    m_restoreFrom = canContinue ? m_snapshot.data() : nullptr;
    m_target = target;
    m_phase = Phase::Advance;
    m_restorePending = true;
}

void Analyzer::beginCandidate(PlayLayer*, int offset) {
    auto& bot = Bot::get();
    m_offset = offset;
    if (offset == 0) m_direction = 0;

    bot.simInputs = bot.replay.inputs;
    auto& moved = bot.simInputs[m_input];
    moved.tick = static_cast<uint32_t>(static_cast<int64_t>(moved.tick) + offset);
    std::stable_sort(bot.simInputs.begin(), bot.simInputs.end(), [](auto const& a, auto const& b) {
        return a.tick < b.tick;
    });

    m_endTick = endTickFor(m_input, offset);
    m_path.clear();
    m_phase = Phase::Candidate;
    m_restoreFrom = m_snapshot.data();
    m_restorePending = true;
}

void Analyzer::finishCandidate(PlayLayer* layer) {
    auto& result = m_results[m_input];
    bool alive = !m_died;

    if (m_offset == 0) {
        // The unshifted input has to reproduce the reference exactly, otherwise
        // nothing measured from this checkpoint can be trusted.
        if (m_died || m_mismatch) {
            result.unreliable = true;
            return this->finishInput(layer);
        }
        m_direction = 1;
        return this->nextOffset(layer);
    }

    if (m_input < paths.size()) {
        paths[m_input].push_back({ m_offset, alive, std::move(m_path) });
    }
    m_path.clear();

    if (alive) {
        if (m_offset > 0) result.right = m_offset;
        else result.left = -m_offset;
        if (result.window() >= maxWindow) {
            result.capped = true;
            return this->finishInput(layer);
        }
        return this->nextOffset(layer);
    }

    if (m_direction > 0) {
        m_direction = -1;
        m_offset = 0;
        return this->nextOffset(layer);
    }
    this->finishInput(layer);
}

void Analyzer::nextOffset(PlayLayer* layer) {
    int next = m_offset + m_direction;
    while (true) {
        if (this->offsetAllowed(next)) {
            return this->beginCandidate(layer, next);
        }
        if (m_direction > 0) {
            m_direction = -1;
            m_offset = 0;
            next = -1;
            continue;
        }
        return this->finishInput(layer);
    }
}

void Analyzer::finishInput(PlayLayer* layer) {
    m_results[m_input].analyzed = true;
    ++m_input;
    this->prepareInput(layer);
}

void Analyzer::finish(PlayLayer* layer, bool keepResults, std::string const& message) {
    auto& bot = Bot::get();
    bot.analyzing = false;
    bot.simInputs.clear();
    bool wasRunning = m_phase != Phase::Starting;
    m_phase = Phase::Idle;
    m_snapshot = nullptr;
    m_hasSnapshot = false;
    m_restorePending = false;
    m_restoreFrom = nullptr;

    if (layer) {
        layer->m_checkpointArray->removeAllObjects();
        if (wasRunning && !m_wasPractice) layer->togglePracticeMode(false);
        layer->resetLevel();
    }
    if (wasRunning) {
        if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
            engine->m_globalChannel->setVolume(m_oldVolume);
        }
    }

    size_t unreliable = 0;
    if (keepResults) {
        bot.replay.analysis = m_results;
        for (auto const& r : m_results) unreliable += r.unreliable ? 1 : 0;
        LStar::computeAsync();
    }
    else {
        paths.clear();
    }
    m_results.clear();

    auto text = unreliable
        ? fmt::format("{} ({} inputs could not be reproduced and were skipped)", message, unreliable)
        : message;
    Notification::create(text, keepResults ? NotificationIcon::Success : NotificationIcon::Warning, 4.f)->show();
}

void Analyzer::restore(PlayLayer* layer, CheckpointObject* checkpoint) {
    Bot::get().injecting = false;
    layer->m_checkpointArray->removeAllObjects();
    if (checkpoint) layer->m_checkpointArray->addObject(checkpoint);
    layer->resetLevel();
}

void Analyzer::onReset(GJBaseGameLayer* layer) {
    // Buttons held across the checkpoint are pressed again, like holding through a respawn.
    auto& bot = Bot::get();
    std::map<std::pair<uint8_t, bool>, bool> held;
    for (auto const& i : bot.simInputs) {
        if (i.tick >= bot.tick) break;
        held[{ i.button, i.player1 }] = i.down;
    }
    for (auto const& [key, isDown] : held) {
        if (!isDown) continue;
        bot.injecting = true;
        layer->handleButton(true, key.first, key.second);
        bot.injecting = false;
    }
}

bool Analyzer::movedPlayerIsP2(GJBaseGameLayer* layer) const {
    auto const& in = Bot::get().replay.inputs;
    return m_input < in.size() && !in[m_input].player1 && layer->m_gameState.m_isDualMode;
}

void Analyzer::onTickEnd(GJBaseGameLayer* layer, uint32_t tick) {
    auto& bot = Bot::get();
    switch (m_phase) {
        case Phase::Reference: {
            if (tick < bot.track.size()) bot.track[tick] = Bot::captureState(layer);
            if (tick >= m_referenceEnd) m_runDone = true;
            break;
        }
        case Phase::Candidate: {
            auto state = Bot::captureState(layer);
            auto const& player = movedPlayerIsP2(layer) ? state.p2 : state.p1;
            m_path.push_back({ player.x, player.y });

            bool known = tick < bot.track.size() && bot.track[tick].valid;
            bool same = known && sameState(state, bot.track[tick]);
            auto const& input = bot.replay.inputs[m_input];
            if (m_offset == 0) {
                if (!same) m_mismatch = true;
            }
            else {
                uint32_t changed = std::max<int64_t>(input.tick, static_cast<int64_t>(input.tick) + m_offset);
                if (tick >= changed && same) {
                    m_converged = true;
                    m_runDone = true;
                }
            }
            if (tick >= m_endTick) m_runDone = true;
            break;
        }
        default:
            break;
    }
}

void Analyzer::onDeath() {
    if (m_phase == Phase::Idle || m_phase == Phase::Starting) return;
    m_died = true;
    m_runDone = true;
}

void Analyzer::onComplete() {
    if (m_phase == Phase::Idle || m_phase == Phase::Starting) return;
    m_completed = true;
    m_runDone = true;
}
