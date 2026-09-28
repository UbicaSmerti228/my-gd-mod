#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <filesystem>
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
};

struct Replay {
    int levelID = 0;
    std::string levelName;
    int tps = 0;
    std::vector<BotInput> inputs;

    uint32_t lastTick() const { return inputs.empty() ? 0 : inputs.back().tick; }
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

    float speed = 1.f;
    bool showOverlay = true;

    // Measured physics rate: ticks per second of level time.
    int measuredTps = 0;

    void setMode(BotMode mode);
    void setSpeed(float speed);

    void onLevelStart(GJGameLevel* level);
    void onReset(GJBaseGameLayer* layer);
    void onTickStart(GJBaseGameLayer* layer);
    void onTickEnd(GJBaseGameLayer* layer);
    void record(bool down, int button, bool player1);

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
