#pragma once

#include "SimController.hpp"

#include <chrono>
#include <map>
#include <vector>

// Finds a route through the level without any replay.
//
// The level is played forward with the current route. When the player dies, the
// finder looks for a fix just before the death: every single press/release it could
// add (at any tick, for each controllable player) is simulated from a checkpoint, and
// a fix is accepted as soon as one survives well past the old death. If no single
// change helps, the most promising candidates (the ones that got furthest) are
// extended with another change, up to a few changes deep (beam search). If that
// fails too, the search window is widened backwards, reopening earlier decisions,
// down to the start of the level. Whatever is found is finally replayed from the
// start without checkpoints, and only a route that passes that way is kept.
class RouteFinder : public SimController {
public:
    static RouteFinder& get();

    bool isActive() const override { return m_phase != Phase::Idle; }
    std::string statusText() const override;

    geode::Result<> start(PlayLayer* layer);
    void cancel(PlayLayer* layer) override;

    void drive(PlayLayer* layer, std::function<void(float)> const& step) override;
    void onTickEnd(GJBaseGameLayer* layer, uint32_t tick) override;
    void onDeath() override;
    void onComplete() override;

private:
    RouteFinder() = default;

    enum class Phase { Idle, Starting, Forward, Evaluate };

    struct Candidate {
        std::vector<BotInput> path;
        // Where the candidate died (or how far it got).
        uint32_t reached = 0;
        // Earliest tick at which it differs from the accepted route.
        uint32_t changedFrom = 0;
    };

    void runForward(PlayLayer* layer, bool fromStart);
    void startSearch(PlayLayer* layer, uint32_t death);
    void nextEvaluation(PlayLayer* layer);
    void evaluationDone(PlayLayer* layer);
    void accept(PlayLayer* layer);
    void widen(PlayLayer* layer);
    void succeed(PlayLayer* layer);
    void finish(PlayLayer* layer, bool found, std::string const& message);

    void storeCheckpoint(PlayLayer* layer, uint32_t tick, bool speculative);
    CheckpointObject* checkpointAtOrBefore(uint32_t tick) const;
    void thinCheckpoints();
    int branchesAt(uint32_t tick) const;
    uint32_t lowerBound(Candidate const& c) const;
    static bool heldAt(std::vector<BotInput> const& path, uint32_t tick, bool player1);

    Phase m_phase = Phase::Idle;
    std::vector<BotInput> m_path;
    std::map<uint32_t, geode::Ref<CheckpointObject>> m_checkpoints;
    std::map<uint32_t, geode::Ref<CheckpointObject>> m_speculative;
    std::vector<uint8_t> m_twoPlayerAt;
    bool m_twoPlayerLevel = false;

    // Current run
    bool m_restorePending = false;
    CheckpointObject* m_restoreFrom = nullptr;
    bool m_fromStart = true;
    bool m_died = false;
    bool m_completed = false;
    uint32_t m_deathTick = 0;

    // Progress
    uint32_t m_frontier = 0;
    uint32_t m_commit = 0;
    uint32_t m_best = 0;
    float m_bestPercent = 0.f;
    size_t m_simulations = 0;
    std::chrono::steady_clock::time_point m_startTime;
    int m_stalledFrames = 0;

    // Search around one obstacle
    uint32_t m_obstacle = 0;
    uint32_t m_window = 0;
    int m_depth = 0;
    std::vector<Candidate> m_beam;
    std::vector<Candidate> m_next;
    size_t m_beamIndex = 0;
    int64_t m_y = 0;
    int m_branch = 0;
    Candidate m_eval;
    uint32_t m_successTick = 0;
};
