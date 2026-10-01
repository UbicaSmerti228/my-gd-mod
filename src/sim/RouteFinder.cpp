#include "RouteFinder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace geode::prelude;

namespace {
    constexpr auto FRAME_BUDGET = std::chrono::milliseconds(14);
    constexpr int MAX_IDLE_STEPS_PER_FRAME = 50;
    constexpr int MAX_STALLED_FRAMES = 900;

    // Checkpoints along the accepted route: dense near the frontier, sparse behind it.
    constexpr uint32_t CHECKPOINT_INTERVAL = 4;
    constexpr uint32_t SPECULATIVE_INTERVAL = 8;
    constexpr uint32_t DENSE_TICKS = 1200;
    constexpr uint32_t SPARSE_INTERVAL = 64;

    // Search around an obstacle.
    constexpr uint32_t WINDOW_START = 90;     // ticks before the death where fixes are tried
    constexpr int MAX_DEPTH = 4;              // changes combined per fix
    constexpr size_t BEAM_WIDTH = 6;          // candidates kept per depth
    constexpr uint32_t SUCCESS_MARGIN = 60;   // ticks past the old death that count as fixed
    constexpr uint32_t COMMIT_TICKS = 1200;   // decisions this far behind the frontier are final
    constexpr uint32_t MAX_TICKS = 240 * 60 * 30;
    constexpr uint32_t COMPLETED = std::numeric_limits<uint32_t>::max();

    std::string routeName(std::string const& level) {
        std::string name;
        for (unsigned char c : level) {
            if (std::isalnum(c) || c == ' ' || c == '_' || c == '-') name += static_cast<char>(c);
        }
        if (name.empty()) name = "level";
        if (name.size() > 50) name.resize(50);
        return name + " route";
    }
}

RouteFinder& RouteFinder::get() {
    static RouteFinder instance;
    return instance;
}

std::string RouteFinder::statusText() const {
    if (m_phase == Phase::Idle) return "";
    if (m_phase == Phase::Starting) return "Route search: resume the game to start";
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startTime).count();
    return fmt::format("Route search: best {:.1f}%  sims {}  depth {}  window {}  {}:{:02}",
        m_bestPercent, m_simulations, m_depth, m_window, elapsed / 60, elapsed % 60);
}

Result<> RouteFinder::start(PlayLayer* layer) {
    if (SimController::active()) return Err("Something is already running");
    if (!layer) return Err("Open the level first");
    if (layer->m_isPlatformer) return Err("Platformer levels are not supported");
    auto& bot = Bot::get();
    geode::log::info("ILL: route search starting");
    this->preparePractice(layer);
    bot.mode = BotMode::Play;
    bot.analyzing = true;
    bot.simInputs.clear();
    m_phase = Phase::Starting;
    return Ok();
}

void RouteFinder::cancel(PlayLayer* layer) {
    if (!isActive()) return;
    finish(layer, false, "Route search cancelled");
}

void RouteFinder::drive(PlayLayer* layer, std::function<void(float)> const& step) {
    auto& bot = Bot::get();

    if (m_phase == Phase::Starting) {
        geode::log::info("ILL: route search: first update");
        this->enterSimulation(layer);
        m_twoPlayerLevel = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
        m_path.clear();
        m_checkpoints.clear();
        m_speculative.clear();
        m_twoPlayerAt.clear();
        m_frontier = m_commit = m_best = 0;
        m_bestPercent = 0.f;
        m_simulations = 0;
        m_depth = 0;
        m_window = WINDOW_START;
        m_stalledFrames = 0;
        m_startTime = std::chrono::steady_clock::now();
        this->runForward(layer, true);
    }

    auto deadline = std::chrono::steady_clock::now() + FRAME_BUDGET;
    bool advanced = false;
    int idleSteps = 0;
    while (isActive() && std::chrono::steady_clock::now() < deadline) {
        if (m_restorePending) {
            m_restorePending = false;
            restore(layer, m_restoreFrom);
            m_died = m_completed = false;
            continue;
        }

        uint32_t ticks = stepOnce(layer, step);
        if (ticks > 1) {
            return finish(layer, false, "The game ran several physics ticks in one update, route search stopped");
        }
        if (ticks == 0) {
            if (++idleSteps > MAX_IDLE_STEPS_PER_FRAME) break;
            continue;
        }
        idleSteps = 0;
        advanced = true;

        uint32_t tick = bot.tick;
        if (tick > MAX_TICKS) return finish(layer, false, "The level is longer than 30 minutes, route search stopped");
        if (tick > m_best) {
            m_best = tick;
            m_bestPercent = layer->getCurrentPercent();
        }

        if (m_phase == Phase::Forward) {
            if (m_died) {
                this->startSearch(layer, m_deathTick);
                continue;
            }
            if (m_completed) {
                // Only a run from the very start, without checkpoints, proves the route.
                if (m_fromStart) return this->succeed(layer);
                this->runForward(layer, true);
                continue;
            }
            if (tick > m_frontier) {
                m_frontier = tick;
                if (m_frontier > COMMIT_TICKS) m_commit = std::max(m_commit, m_frontier - COMMIT_TICKS);
            }
            if (tick % CHECKPOINT_INTERVAL == 0 && !m_checkpoints.contains(tick)) {
                this->storeCheckpoint(layer, tick, false);
                if (tick % SPARSE_INTERVAL == 0) this->thinCheckpoints();
            }
        }
        else if (m_phase == Phase::Evaluate) {
            if (m_died || m_completed || tick >= m_successTick) {
                m_eval.reached = m_completed ? COMPLETED : m_died ? m_deathTick : tick;
                this->evaluationDone(layer);
                continue;
            }
            if (tick % SPECULATIVE_INTERVAL == 0 && tick > m_eval.changedFrom && !m_speculative.contains(tick)) {
                this->storeCheckpoint(layer, tick, true);
            }
        }
    }

    if (!isActive()) return;
    if (advanced || m_restorePending) {
        m_stalledFrames = 0;
    }
    else if (++m_stalledFrames > MAX_STALLED_FRAMES) {
        finish(layer, false, "The level stopped advancing, route search stopped");
    }
}

void RouteFinder::runForward(PlayLayer*, bool fromStart) {
    auto& bot = Bot::get();
    bot.simInputs = m_path;
    if (fromStart) {
        m_checkpoints.clear();
        m_restoreFrom = nullptr;
    }
    else {
        m_restoreFrom = m_checkpoints.empty() ? nullptr : m_checkpoints.rbegin()->second.data();
    }
    m_fromStart = m_restoreFrom == nullptr;
    m_phase = Phase::Forward;
    m_restorePending = true;
}

void RouteFinder::startSearch(PlayLayer* layer, uint32_t death) {
    geode::log::info("ILL: route search: died at tick {}, searching a fix", death);
    m_obstacle = death;
    m_successTick = death + SUCCESS_MARGIN;
    m_depth = 1;
    m_beam = { Candidate { m_path, death, COMPLETED } };
    m_next.clear();
    m_beamIndex = 0;
    m_y = death;
    m_branch = 0;
    this->nextEvaluation(layer);
}

uint32_t RouteFinder::lowerBound(Candidate const& c) const {
    uint32_t reach = c.reached == COMPLETED ? m_obstacle : c.reached;
    uint32_t low = reach > m_window ? reach - m_window : 0;
    // Deeper candidates only add changes after their own: an earlier change would
    // throw theirs away and repeat a shallower candidate.
    if (c.changedFrom != COMPLETED) low = std::max(low, c.changedFrom + 1);
    return std::max(low, m_commit);
}

int RouteFinder::branchesAt(uint32_t tick) const {
    return tick < m_twoPlayerAt.size() && m_twoPlayerAt[tick] ? 2 : 1;
}

bool RouteFinder::heldAt(std::vector<BotInput> const& path, uint32_t tick, bool player1) {
    bool held = false;
    for (auto const& i : path) {
        if (i.tick >= tick) break;
        if (i.player1 == player1) held = i.down;
    }
    return held;
}

void RouteFinder::nextEvaluation(PlayLayer* layer) {
    auto& bot = Bot::get();
    while (true) {
        if (m_beamIndex >= m_beam.size()) {
            if (m_depth >= MAX_DEPTH || m_next.empty()) return this->widen(layer);
            // Keep the candidates that got furthest and try one more change on each.
            std::stable_sort(m_next.begin(), m_next.end(), [](auto const& a, auto const& b) {
                return a.reached > b.reached;
            });
            if (m_next.size() > BEAM_WIDTH) m_next.resize(BEAM_WIDTH);
            m_beam = std::move(m_next);
            m_next.clear();
            m_beamIndex = 0;
            m_depth++;
            m_y = m_beam[0].reached;
            m_branch = 0;
            continue;
        }

        auto const& base = m_beam[m_beamIndex];
        if (++m_branch > branchesAt(static_cast<uint32_t>(m_y))) {
            m_branch = 1;
            --m_y;
        }
        if (m_y < static_cast<int64_t>(lowerBound(base))) {
            if (++m_beamIndex < m_beam.size()) {
                m_y = m_beam[m_beamIndex].reached;
                m_branch = 0;
            }
            continue;
        }

        // Candidate: the base route up to tick y, then flip the chosen player's button at y.
        auto y = static_cast<uint32_t>(m_y);
        bool player1 = m_branch == 1;
        Candidate c;
        c.path.reserve(base.path.size() + 1);
        for (auto const& i : base.path) {
            if (i.tick < y) c.path.push_back(i);
        }
        c.path.push_back({ y, 1, player1, !heldAt(c.path, y, player1) });
        c.changedFrom = std::min(base.changedFrom, y);

        m_eval = std::move(c);
        bot.simInputs = m_eval.path;
        m_speculative.clear();
        m_restoreFrom = checkpointAtOrBefore(m_eval.changedFrom);
        m_fromStart = m_restoreFrom == nullptr;
        m_phase = Phase::Evaluate;
        m_restorePending = true;
        return;
    }
}

void RouteFinder::evaluationDone(PlayLayer* layer) {
    ++m_simulations;
    if (m_eval.reached == COMPLETED || m_eval.reached >= m_successTick) {
        return this->accept(layer);
    }
    m_speculative.clear();
    m_next.push_back(m_eval);
    this->nextEvaluation(layer);
}

void RouteFinder::accept(PlayLayer* layer) {
    bool completed = m_eval.reached == COMPLETED;
    m_path = std::move(m_eval.path);
    Bot::get().simInputs = m_path;

    // Checkpoints after the change belong to the old route; the ones taken during the
    // successful evaluation belong to the new one.
    m_checkpoints.erase(m_checkpoints.upper_bound(m_eval.changedFrom), m_checkpoints.end());
    for (auto& [tick, cp] : m_speculative) m_checkpoints[tick] = cp;
    m_speculative.clear();
    m_beam.clear();
    m_next.clear();
    m_window = WINDOW_START;
    m_depth = 0;
    m_phase = Phase::Forward;
    m_frontier = Bot::get().tick;

    if (completed) {
        if (m_fromStart) return this->succeed(layer);
        return this->runForward(layer, true);
    }
    // The evaluation run is alive past the obstacle: keep playing from here.
}

void RouteFinder::widen(PlayLayer* layer) {
    uint32_t span = m_obstacle > m_commit ? m_obstacle - m_commit : 0;
    if (m_window < span) {
        m_window = std::min(m_window * 2, span);
    }
    else if (m_commit > 0) {
        // Reopen decisions that were considered final.
        m_commit = m_commit > COMMIT_TICKS ? m_commit - COMMIT_TICKS : 0;
        m_window = std::max(m_window, m_obstacle - m_commit);
    }
    else {
        return finish(layer, false, fmt::format(
            "No route found past {:.1f}%: no combination of up to {} presses/releases before the death works",
            m_bestPercent, MAX_DEPTH));
    }

    m_depth = 1;
    m_beam = { Candidate { m_path, m_obstacle, COMPLETED } };
    m_next.clear();
    m_beamIndex = 0;
    m_y = m_obstacle;
    m_branch = 0;
    this->nextEvaluation(layer);
}

void RouteFinder::storeCheckpoint(PlayLayer* layer, uint32_t tick, bool speculative) {
    auto checkpoint = layer->createCheckpoint();
    if (!checkpoint) return;
    (speculative ? m_speculative : m_checkpoints)[tick] = checkpoint;
}

CheckpointObject* RouteFinder::checkpointAtOrBefore(uint32_t tick) const {
    auto it = m_checkpoints.upper_bound(tick);
    if (it == m_checkpoints.begin()) return nullptr;
    return std::prev(it)->second.data();
}

void RouteFinder::thinCheckpoints() {
    for (auto it = m_checkpoints.begin(); it != m_checkpoints.end();) {
        if (it->first + DENSE_TICKS < m_frontier && it->first % SPARSE_INTERVAL != 0) {
            it = m_checkpoints.erase(it);
        }
        else {
            ++it;
        }
    }
}

void RouteFinder::succeed(PlayLayer* layer) {
    auto& bot = Bot::get();
    Replay replay;
    replay.levelID = layer->m_level ? layer->m_level->m_levelID.value() : 0;
    replay.levelName = layer->m_level ? std::string(layer->m_level->m_levelName.c_str()) : "";
    replay.tps = bot.replay.effectiveTps();
    replay.inputs = m_path;
    auto simulations = m_simulations;

    finish(layer, true, "");
    bot.replay = std::move(replay);
    bot.clearAnalysis();
    bot.track.clear();
    bot.mode = BotMode::Off;

    auto name = routeName(bot.replay.levelName);
    auto saved = bot.save(name);
    if (saved) bot.markSaved(name);
    else bot.unsaved = true;
    auto text = saved
        ? fmt::format("Route found: {} inputs after {} simulations, saved as \"{}\"", m_path.size(), simulations, name)
        : fmt::format("Route found: {} inputs after {} simulations (not saved: {})", m_path.size(), simulations, saved.unwrapErr());
    Notification::create(text, NotificationIcon::Success, 6.f)->show();
}

void RouteFinder::finish(PlayLayer* layer, bool found, std::string const& message) {
    m_phase = Phase::Idle;
    m_checkpoints.clear();
    m_speculative.clear();
    m_beam.clear();
    m_next.clear();
    m_restorePending = false;
    m_restoreFrom = nullptr;
    this->leaveSimulation(layer);
    // Without a route there is nothing to play back.
    if (!found) Bot::get().mode = BotMode::Off;
    if (!message.empty()) {
        Notification::create(message, found ? NotificationIcon::Success : NotificationIcon::Warning, 6.f)->show();
    }
}

void RouteFinder::onTickEnd(GJBaseGameLayer* layer, uint32_t tick) {
    if (tick >= m_twoPlayerAt.size()) m_twoPlayerAt.resize(tick + 1024, 0);
    m_twoPlayerAt[tick] = m_twoPlayerLevel && layer->m_gameState.m_isDualMode;
}

void RouteFinder::onDeath() {
    if (m_phase != Phase::Forward && m_phase != Phase::Evaluate) return;
    if (m_died) return;
    m_died = true;
    m_deathTick = Bot::get().tick;
}

void RouteFinder::onComplete() {
    if (m_phase != Phase::Forward && m_phase != Phase::Evaluate) return;
    m_completed = true;
}
