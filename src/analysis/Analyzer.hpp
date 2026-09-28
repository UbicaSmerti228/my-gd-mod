#pragma once

#include "../Bot.hpp"

#include <Geode/Geode.hpp>

#include <functional>
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
class Analyzer {
public:
    static Analyzer& get();

    // Largest window searched for (ticks); anything wider is reported as capped.
    int maxWindow = 20;
    // Ticks after the next input the shifted run must survive.
    int settleTicks = 10;

    bool isActive() const { return m_phase != Phase::Idle; }
    float progress() const;
    std::string statusText() const;

    // Alternative trajectories found for each input, parallel to the replay inputs.
    std::vector<std::vector<TrajectoryPath>> paths;
    void clearPaths();

    geode::Result<> start(PlayLayer* layer);
    void cancel(PlayLayer* layer);

    // Called instead of the normal game update while active; `step` runs one
    // original GJBaseGameLayer::update.
    void drive(PlayLayer* layer, std::function<void(float)> const& step);

    void onTickEnd(GJBaseGameLayer* layer, uint32_t tick);
    void onReset(GJBaseGameLayer* layer);
    void onDeath();
    void onComplete();

private:
    Analyzer() = default;

    enum class Phase { Idle, Starting, Reference, Advance, Candidate };

    void beginReference(PlayLayer* layer);
    void prepareInput(PlayLayer* layer);
    void beginAdvance(PlayLayer* layer, uint32_t target);
    void beginCandidate(PlayLayer* layer, int offset);
    void finishCandidate(PlayLayer* layer);
    void finishInput(PlayLayer* layer);
    void nextOffset(PlayLayer* layer);
    void finish(PlayLayer* layer, bool keepResults, std::string const& message);

    void restore(PlayLayer* layer, CheckpointObject* checkpoint);
    bool offsetAllowed(int offset) const;
    uint32_t snapshotTickFor(size_t index) const;
    uint32_t endTickFor(size_t index, int offset) const;
    bool movedPlayerIsP2(GJBaseGameLayer* layer) const;

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
    int m_offset = 0;
    int m_direction = 0;
    uint32_t m_endTick = 0;
    uint32_t m_referenceEnd = 0;
    std::vector<cocos2d::CCPoint> m_path;

    bool m_wasPractice = false;
    float m_oldVolume = 1.f;
};
