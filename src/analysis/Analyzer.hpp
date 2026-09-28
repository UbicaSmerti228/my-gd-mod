#pragma once

#include "../sim/SimController.hpp"

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

// One alternative trajectory: where the player goes when an input is moved by `offset` ticks.
struct TrajectoryPath {
    int offset = 0;
    bool alive = false;
    std::vector<cocos2d::CCPoint> points;
};

// Finds the frame window of every input in the loaded replay by brute force.
//
// For each input the level is restored from a checkpoint taken shortly before it,
// the input is moved by +-1, +-2, ... ticks, and the level is simulated tick by tick
// with every other input left where it was. A shifted input works if the player
// survives until shortly after the next input, or rejoins the reference trajectory
// earlier. Before searching, the unshifted input is replayed from the same
// checkpoint: if that does not reproduce the reference run exactly, the result
// for that input is marked unreliable instead of being trusted.
//
// With CBF measurement on, each window edge is then bisected to 1/16 tick by placing
// the input between ticks.
//
// In optimize mode, after the analysis every input is moved to the middle of its
// window, the new replay is checked from the start (moves that break it are undone),
// and the result is analyzed again.
class Analyzer : public SimController {
public:
    static Analyzer& get();

    // Largest window searched for (ticks); anything wider is reported as capped.
    int maxWindow = 20;
    // Ticks after the next input the shifted run must survive.
    int settleTicks = 10;
    // Also measure fractional (Click Between Frames) windows.
    bool measureCbf = true;

    bool isActive() const override { return m_phase != Phase::Idle; }
    std::string statusText() const override;
    float progress() const;

    // Alternative trajectories found for each input, parallel to the replay inputs.
    std::vector<std::vector<TrajectoryPath>> paths;
    void clearPaths();

    geode::Result<> start(PlayLayer* layer, bool optimize);
    void cancel(PlayLayer* layer) override;

    void drive(PlayLayer* layer, std::function<void(float)> const& step) override;
    void onTickEnd(GJBaseGameLayer* layer, uint32_t tick) override;
    void onDeath() override;
    void onComplete() override;

private:
    Analyzer() = default;

    enum class Phase { Idle, Starting, Reference, Advance, Candidate, Verify };

    void beginReference(PlayLayer* layer);
    void prepareInput(PlayLayer* layer);
    void beginAdvance(PlayLayer* layer, uint32_t target);
    void beginCandidate(PlayLayer* layer, int offset);
    void beginCandidateAt(PlayLayer* layer, double offset);
    void finishCandidate(PlayLayer* layer);
    void finishInput(PlayLayer* layer);
    void completeInput(PlayLayer* layer);
    void nextOffset(PlayLayer* layer);
    void startRefine(PlayLayer* layer);
    void refineStep(PlayLayer* layer);
    void finishRefine(PlayLayer* layer, bool alive);
    void analysisDone(PlayLayer* layer);
    void beginVerify(PlayLayer* layer);
    void finishVerify(PlayLayer* layer);
    void finish(PlayLayer* layer, bool keepResults, std::string const& message);

    bool offsetAllowed(int offset) const;
    uint32_t snapshotTickFor(size_t index) const;
    uint32_t endTickFor(size_t index, double offset) const;
    bool movedPlayerIsP2(GJBaseGameLayer* layer) const;
    std::vector<BotInput> optimizedInputs() const;

    Phase m_phase = Phase::Idle;
    size_t m_input = 0;
    std::vector<InputAnalysis> m_results;

    geode::Ref<CheckpointObject> m_snapshot;
    uint32_t m_snapshotTick = 0;
    bool m_hasSnapshot = false;
    uint32_t m_target = 0;

    // Current run
    bool m_restorePending = false;
    CheckpointObject* m_restoreFrom = nullptr;
    bool m_runDone = false;
    bool m_died = false;
    bool m_completed = false;
    bool m_converged = false;
    bool m_mismatch = false;
    uint32_t m_deathTick = 0;
    int m_offset = 0;
    double m_offsetF = 0.0;
    int m_direction = 0;
    bool m_rightDeath = false;
    bool m_leftDeath = false;
    uint32_t m_endTick = 0;
    uint32_t m_referenceEnd = 0;
    std::vector<cocos2d::CCPoint> m_path;
    int m_stalledFrames = 0;

    // CBF refinement: bisecting the fractional position of one window edge.
    bool m_refining = false;
    int m_refineSide = 0;
    int m_refineIter = 0;
    double m_refineAlive = 0.0;
    double m_refineDead = 0.0;
    double m_rightEdge = 0.0;
    double m_leftEdge = 0.0;

    // Optimizer
    bool m_optimize = false;
    int m_stage = 1;
    std::vector<int> m_shift;
    size_t m_moved = 0;
};
