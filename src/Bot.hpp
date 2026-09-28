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
    // Fraction of the tick at which the input happens (Click Between Frames style).
    // Only the analyzer sets this; replays store whole ticks.
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

    // An input the analyzer placed between ticks, waiting for the player update of
    // this tick to split it (see hooks.cpp).
    struct PendingSplit {
        bool active = false;
        BotInput input {};
    };
    PendingSplit split;

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
    void record(bool down, int button, bool player1);

    static TickState captureState(GJBaseGameLayer* layer);

    // Replays live in <save dir>/replays/<name>.ilr
    static std::filesystem::path replayDir();
    static std::vector<std::string> listReplays();
    geode::Result<> save(std::string const& name) const;
    geode::Result<> load(std::string const& name);
    static geode::Result<> remove(std::string const& name);

private:
    Bot();

    uint32_t m_tpsStartTick = 0;
    double m_tpsStartTime = 0.0;
};
