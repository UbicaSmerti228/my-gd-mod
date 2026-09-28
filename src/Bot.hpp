#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

enum class BotMode { Off, Record, Play };

// One button event, pinned to a physics tick rather than to wall-clock time
// or to a rendered frame. That keeps playback identical no matter how much the
// FPS drops while a video is being recorded.
struct BotInput {
    uint32_t tick;
    uint8_t button;
    bool player1;
    bool down;
    // Fraction of the tick at which the input happens (Click Between Frames style):
    // recorded in CBF mode, and used by the analyzer to place inputs between ticks.
    float subtick = 0.f;
};

// Result of the frame window analysis for one input.
struct InputAnalysis {
    // How many ticks the input can be moved earlier / later and still work.
    int left = 0;
    int right = 0;
    bool analyzed = false;
    // The search stopped at the configured maximum, the real window may be wider.
    bool capped = false;
    // Replaying this input unchanged did not reproduce the reference run, so the
    // window for it cannot be trusted.
    bool unreliable = false;
    // Window width in fractional ticks when inputs can land between ticks (CBF),
    // 0 if it was not measured.
    float cbf = 0.f;
    // Player x position at the input in the reference run, 0 if unknown. Used to map
    // deaths and start positions of normal attempts to inputs.
    float x = 0.f;

    int window() const { return left + right + 1; }
};

struct PlayerState {
    float x = 0.f;
    float y = 0.f;
    double yVelocity = 0.0;
};

struct TickState {
    PlayerState p1;
    PlayerState p2;
    bool dual = false;
    bool valid = false;
};

struct Replay {
    int levelID = 0;
    std::string levelName;
    int tps = 0;
    std::vector<BotInput> inputs;
    // Parallel to `inputs` once an analysis has been run, empty otherwise.
    std::vector<InputAnalysis> analysis;
    // Running L* and difficulty share after each input, parallel to `inputs`.
    std::vector<double> lstar;
    std::vector<double> lstarShare;

    // Window used for display and L*: the CBF width when enabled and measured.
    double windowOf(size_t index, bool cbf) const {
        auto const& a = analysis[index];
        return cbf && a.cbf > 0.f ? a.cbf : a.window();
    }

    uint32_t lastTick() const { return inputs.empty() ? 0 : inputs.back().tick; }
    bool hasAnalysis() const { return !analysis.empty() && analysis.size() == inputs.size(); }
    int effectiveTps() const { return tps > 0 ? tps : 240; }
};

class Bot {
public:
    static Bot& get();

    BotMode mode = BotMode::Off;
    Replay replay;

    // Physics ticks since the attempt started (restored from checkpoints in practice).
    uint32_t tick = 0;
    // Next input to play back.
    size_t playIndex = 0;
    // Set while the bot itself calls handleButton, so the hook lets it through.
    bool injecting = false;

    // While the analyzer runs, playback uses its modified copy of the inputs.
    bool analyzing = false;
    std::vector<BotInput> simInputs;
    std::vector<BotInput> const& playbackInputs() const { return analyzing ? simInputs : replay.inputs; }

    // Player states per tick from the last clean playback, used for the
    // future / past trajectory lines.
    std::vector<TickState> track;

    float speed = 1.f;
    bool showOverlay = true;
    // Frame window counter, CPS and L* labels, and ring markers.
    bool showCounter = true;
    // Past / future / alternative trajectory lines.
    bool showPaths = true;
    bool playSounds = true;
    // Show and score CBF (fractional) windows instead of whole ticks.
    bool useCbf = true;

    // Record and replay inputs between ticks, for players using Click Between Frames
    // (or the game's Click Between Steps).
    bool cbfMode = false;

    // Inputs of the current tick that happen between ticks, waiting for the player
    // update of this tick to split its step (see hooks.cpp).
    std::vector<BotInput> splits;
    // Set while GJBaseGameLayer::processCommands runs.
    bool inTick = false;
    // The last level reset restored the players from a full state snapshot.
    bool restoredPlayers = false;
    // Replay changed since it was last saved or loaded.
    bool unsaved = false;
    // Name the current replay was loaded from or saved as.
    std::string replayName;

    // Measured physics rate: ticks per second of level time.
    int measuredTps = 0;

    // Called on the main thread whenever playback applies input `index`.
    std::function<void(size_t index)> onInputPlayed;

    void setMode(BotMode mode);
    void setSpeed(float speed);
    void clearAnalysis();

    void onLevelStart(GJGameLevel* level);
    void onReset(GJBaseGameLayer* layer);
    void onTickStart(GJBaseGameLayer* layer);
    void onTickEnd(GJBaseGameLayer* layer);
    void record(GJBaseGameLayer* layer, bool down, int button, bool player1);
    void applySplitsNow(GJBaseGameLayer* layer);

    static TickState captureState(GJBaseGameLayer* layer);

    // Replays live in <save dir>/replays/<name>.ilr
    static std::filesystem::path replayDir();
    static std::vector<std::string> listReplays();
    geode::Result<> save(std::string const& name) const;
    void markSaved(std::string const& name);
    geode::Result<> load(std::string const& name);
    // Reads a replay file without loading it.
    static geode::Result<Replay> readReplay(std::string const& name);
    static geode::Result<> remove(std::string const& name);

private:
    Bot();

    uint32_t m_tpsStartTick = 0;
    double m_tpsStartTime = 0.0;

    // Player x at the start of this tick and how far it moved during the last one,
    // to tell how much of the tick had passed when an input came in (CBF mode).
    float m_tickStartX[2] = { 0.f, 0.f };
    float m_lastStepX[2] = { 0.f, 0.f };
};
