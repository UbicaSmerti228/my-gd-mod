#include "Analyzer.hpp"
#include "LStar.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    // Bisection steps per CBF window edge: 1/16 tick precision.
    constexpr int CBF_STEPS = 4;

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

    void sortByTick(std::vector<BotInput>& inputs) {
        std::stable_sort(inputs.begin(), inputs.end(), [](auto const& a, auto const& b) {
            return a.tick < b.tick;
        });
    }
}

Analyzer& Analyzer::get() {
    static Analyzer instance;
    return instance;
}

float Analyzer::progress() const {
    auto count = Bot::get().replay.inputs.size();
    if (m_phase != Phase::Advance && m_phase != Phase::Candidate) return 0.f;
    if (count == 0) return 0.f;
    return static_cast<float>(m_input) / static_cast<float>(count);
}

std::string Analyzer::statusText() const {
    auto count = Bot::get().replay.inputs.size();
    auto stage = m_optimize ? fmt::format("Optimize {}/2: ", m_stage) : std::string("Analysis: ");
    switch (m_phase) {
        case Phase::Idle: return "";
        case Phase::Starting: return stage + "resume the game to start";
        case Phase::Reference: return fmt::format("{}reference run, tick {}", stage, Bot::get().tick);
        case Phase::Verify: return fmt::format("{}checking the optimized replay, tick {}", stage, Bot::get().tick);
        case Phase::Advance:
        case Phase::Candidate:
            if (m_refining) {
                return fmt::format("{}{:.1f}%  input {}/{}  CBF edge {:+.3f}",
                    stage, progress() * 100.f, m_input + 1, count, m_offsetF);
            }
            return fmt::format("{}{:.1f}%  input {}/{}  offset {:+d}",
                stage, progress() * 100.f, m_input + 1, count, m_offset);
    }
    return "";
}

void Analyzer::clearPaths() {
    paths.clear();
}

Result<> Analyzer::start(PlayLayer* layer, bool optimize) {
    auto& bot = Bot::get();
    if (SimController::active()) return Err("Something is already running");
    if (!layer) return Err("Open the level first");
    if (bot.replay.inputs.empty()) return Err("Load or record a replay first");
    int levelID = layer->m_level ? layer->m_level->m_levelID.value() : 0;
    if (bot.replay.levelID && levelID && bot.replay.levelID != levelID) {
        return Err("This replay was recorded on another level");
    }
    if (layer->m_isPlatformer) return Err("Platformer levels are not supported");

    maxWindow = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("max-window", 20)), 13, 120);
    settleTicks = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("settle-ticks", 10)), 0, 240);
    measureCbf = Mod::get()->getSavedValue<bool>("cbf-analysis", true);

    bot.mode = BotMode::Play;
    bot.analyzing = true;
    bot.simInputs = bot.replay.inputs;
    paths.clear();
    m_results.clear();
    m_optimize = optimize;
    m_stage = 1;
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
        this->enterSimulation(layer);
        m_stalledFrames = 0;
        this->beginReference(layer);
    }

    auto deadline = std::chrono::steady_clock::now() + FRAME_BUDGET;
    bool advanced = false;
    int idleSteps = 0;
    while (isActive() && std::chrono::steady_clock::now() < deadline) {
        if (m_restorePending) {
            m_restorePending = false;
            restore(layer, m_restoreFrom);
            m_runDone = m_died = m_completed = m_converged = m_mismatch = false;
            continue;
        }

        uint32_t ticks = stepOnce(layer, step);
        if (ticks > 1) {
            return finish(layer, false, "The game ran several physics ticks in one update, analysis stopped");
        }
        if (ticks == 0) {
            if (++idleSteps > MAX_IDLE_STEPS_PER_FRAME) break;
            continue;
        }
        idleSteps = 0;
        advanced = true;

        if (m_phase == Phase::Advance && !m_runDone && bot.tick == m_target) {
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
                    return finish(layer, false, fmt::format("The replay dies at tick {}, analysis needs a replay that passes", m_deathTick));
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
                return finish(layer, false, fmt::format("The replay did not reproduce (died at tick {})", m_deathTick));
            case Phase::Candidate:
                this->finishCandidate(layer);
                break;
            case Phase::Verify:
                this->finishVerify(layer);
                break;
            default:
                break;
        }
    }

    if (!isActive()) return;
    if (advanced || m_restorePending) {
        m_stalledFrames = 0;
    }
    else if (++m_stalledFrames > MAX_STALLED_FRAMES) {
        finish(layer, false, "The level stopped advancing, analysis stopped");
    }
}

void Analyzer::beginReference(PlayLayer*) {
    auto& bot = Bot::get();
    bot.simInputs = bot.replay.inputs;
    m_phase = Phase::Reference;
    m_referenceEnd = bot.replay.lastTick() + 240;
    bot.track.assign(m_referenceEnd + 1, {});
    m_hasSnapshot = false;
    m_snapshot = nullptr;
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

uint32_t Analyzer::endTickFor(size_t index, double offset) const {
    auto const& bot = Bot::get();
    auto const& in = bot.replay.inputs;
    auto moved = static_cast<uint32_t>(std::max(0.0, std::ceil(in[index].tick + offset)));
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
    if (m_input >= in.size()) return this->analysisDone(layer);

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

void Analyzer::beginCandidate(PlayLayer* layer, int offset) {
    m_offset = offset;
    if (offset == 0) {
        m_direction = 0;
        m_rightDeath = false;
        m_leftDeath = false;
        m_refining = false;
    }
    this->beginCandidateAt(layer, offset);
}

void Analyzer::beginCandidateAt(PlayLayer*, double offset) {
    auto& bot = Bot::get();
    m_offsetF = offset;

    bot.simInputs = bot.replay.inputs;
    auto& moved = bot.simInputs[m_input];
    double time = moved.tick + offset;
    double whole = std::floor(time);
    moved.tick = static_cast<uint32_t>(std::max(0.0, whole));
    moved.subtick = static_cast<float>(time - whole);
    if (moved.subtick < 1e-4f) moved.subtick = 0.f;
    sortByTick(bot.simInputs);

    m_endTick = endTickFor(m_input, offset);
    m_path.clear();
    m_phase = Phase::Candidate;
    m_restoreFrom = m_snapshot.data();
    m_restorePending = true;
}

void Analyzer::finishCandidate(PlayLayer* layer) {
    auto& result = m_results[m_input];
    bool alive = !m_died;
    if (m_refining) return this->finishRefine(layer, alive);

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
        m_rightDeath = true;
        m_direction = -1;
        m_offset = 0;
        return this->nextOffset(layer);
    }
    m_leftDeath = true;
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
    auto& result = m_results[m_input];
    if (measureCbf && !result.unreliable && !result.capped) {
        if (m_rightDeath || m_leftDeath) return this->startRefine(layer);
        // Both sides stopped at a neighbouring input of the same button, not at a death.
        result.cbf = static_cast<float>(result.window());
    }
    this->completeInput(layer);
}

void Analyzer::completeInput(PlayLayer* layer) {
    m_results[m_input].analyzed = true;
    m_refining = false;
    ++m_input;
    this->prepareInput(layer);
}

// The whole-tick search gives, per side, the last offset that works and the first that
// kills. With inputs allowed between ticks the real edge lies somewhere in between;
// bisecting the fractional input time finds it. Sides that ended at a neighbouring
// input instead of a death are assumed to be half a tick wide.
void Analyzer::startRefine(PlayLayer* layer) {
    auto const& result = m_results[m_input];
    m_refining = true;
    m_rightEdge = result.right + 0.5;
    m_leftEdge = -result.left - 0.5;
    m_refineIter = 0;
    if (m_rightDeath) {
        m_refineSide = 1;
        m_refineAlive = result.right;
        m_refineDead = result.right + 1;
    }
    else {
        m_refineSide = -1;
        m_refineAlive = -result.left;
        m_refineDead = -result.left - 1;
    }
    this->refineStep(layer);
}

void Analyzer::refineStep(PlayLayer* layer) {
    if (m_refineIter < CBF_STEPS) {
        return this->beginCandidateAt(layer, (m_refineAlive + m_refineDead) * 0.5);
    }

    double edge = (m_refineAlive + m_refineDead) * 0.5;
    if (m_refineSide > 0) {
        m_rightEdge = edge;
        if (m_leftDeath) {
            auto const& result = m_results[m_input];
            m_refineSide = -1;
            m_refineIter = 0;
            m_refineAlive = -result.left;
            m_refineDead = -result.left - 1;
            return this->refineStep(layer);
        }
    }
    else {
        m_leftEdge = edge;
    }

    auto& result = m_results[m_input];
    result.cbf = static_cast<float>(std::max(m_rightEdge - m_leftEdge, 1.0 / (1 << CBF_STEPS)));
    this->completeInput(layer);
}

void Analyzer::finishRefine(PlayLayer* layer, bool alive) {
    m_path.clear();
    if (alive) m_refineAlive = m_offsetF;
    else m_refineDead = m_offsetF;
    ++m_refineIter;
    this->refineStep(layer);
}

void Analyzer::analysisDone(PlayLayer* layer) {
    auto& bot = Bot::get();
    // Remember where each input happens in the level: the forecast and the heatmap
    // map deaths and start positions to inputs by x position.
    for (size_t i = 0; i < m_results.size(); ++i) {
        auto tick = bot.replay.inputs[i].tick;
        if (tick < bot.track.size() && bot.track[tick].valid) m_results[i].x = bot.track[tick].p1.x;
    }

    if (!m_optimize || m_stage != 1) {
        return finish(layer, true, m_optimize ? "Optimization finished" : "Analysis finished");
    }

    // Move every trustworthy input to the middle of its window.
    m_shift.assign(m_results.size(), 0);
    m_moved = 0;
    for (size_t i = 0; i < m_results.size(); ++i) {
        auto const& r = m_results[i];
        if (!r.analyzed || r.unreliable || r.capped) continue;
        int center = static_cast<int>(std::lround((r.right - r.left) / 2.0));
        m_shift[i] = center;
        if (center != 0) ++m_moved;
    }
    if (m_moved == 0) {
        return finish(layer, true, "Every input is already in the middle of its window");
    }
    this->beginVerify(layer);
}

std::vector<BotInput> Analyzer::optimizedInputs() const {
    auto inputs = Bot::get().replay.inputs;
    for (size_t i = 0; i < inputs.size() && i < m_shift.size(); ++i) {
        inputs[i].tick = static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(inputs[i].tick) + m_shift[i]));
    }
    sortByTick(inputs);
    return inputs;
}

void Analyzer::beginVerify(PlayLayer*) {
    auto& bot = Bot::get();
    bot.simInputs = optimizedInputs();
    m_phase = Phase::Verify;
    m_referenceEnd = bot.simInputs.empty() ? 240 : bot.simInputs.back().tick + 240;
    m_restoreFrom = nullptr;
    m_restorePending = true;
}

void Analyzer::finishVerify(PlayLayer* layer) {
    auto& bot = Bot::get();
    if (!m_died) {
        // The centered replay passes: make it the replay and measure it again.
        size_t moved = 0;
        for (int s : m_shift) moved += s != 0 ? 1 : 0;
        bot.replay.inputs = optimizedInputs();
        bot.replay.analysis.clear();
        bot.replay.lstar.clear();
        bot.replay.lstarShare.clear();
        m_stage = 2;
        Notification::create(fmt::format("{} inputs centered, measuring the new replay", moved), NotificationIcon::Info)->show();
        return this->beginReference(layer);
    }

    // Undo the latest move at or before the death and try again.
    auto const& in = bot.replay.inputs;
    long undo = -1;
    int64_t undoTick = -1;
    for (size_t i = 0; i < in.size() && i < m_shift.size(); ++i) {
        if (m_shift[i] == 0) continue;
        int64_t tick = static_cast<int64_t>(in[i].tick) + m_shift[i];
        if (tick <= static_cast<int64_t>(m_deathTick) && tick >= undoTick) {
            undoTick = tick;
            undo = static_cast<long>(i);
        }
    }
    if (undo < 0) {
        return finish(layer, true, "The centered replay does not pass; kept the original analysis");
    }
    m_shift[undo] = 0;
    this->beginVerify(layer);
}

void Analyzer::finish(PlayLayer* layer, bool keepResults, std::string const& message) {
    auto& bot = Bot::get();
    m_phase = Phase::Idle;
    m_snapshot = nullptr;
    m_hasSnapshot = false;
    m_restorePending = false;
    m_restoreFrom = nullptr;
    this->leaveSimulation(layer);

    size_t unreliable = 0;
    if (keepResults && m_results.size() == bot.replay.inputs.size()) {
        bot.replay.analysis = m_results;
        for (auto const& r : m_results) unreliable += r.unreliable ? 1 : 0;
        LStar::computeAsync();
    }
    else if (!keepResults) {
        paths.clear();
    }
    m_results.clear();
    m_shift.clear();

    auto text = unreliable
        ? fmt::format("{} ({} inputs could not be reproduced and were skipped)", message, unreliable)
        : message;
    Notification::create(text, keepResults ? NotificationIcon::Success : NotificationIcon::Warning, 4.f)->show();
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
        case Phase::Verify: {
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
            if (!m_refining && m_offset == 0) {
                if (!same) m_mismatch = true;
            }
            else {
                auto moved = static_cast<int64_t>(std::ceil(input.tick + m_offsetF));
                int64_t changed = std::max<int64_t>(input.tick, moved);
                if (static_cast<int64_t>(tick) >= changed && same) {
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
    m_deathTick = Bot::get().tick;
}

void Analyzer::onComplete() {
    if (m_phase == Phase::Idle || m_phase == Phase::Starting) return;
    m_completed = true;
    m_runDone = true;
}
