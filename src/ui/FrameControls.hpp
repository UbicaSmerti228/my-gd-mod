#pragma once

#include <Geode/Geode.hpp>

#include <deque>

// On-screen controls for building macros tick by tick: freeze / resume, advance one
// tick, and rewind about a second (recording only). The same actions are on the
// keyboard: F, V and C. Rewinding uses the bot's own checkpoints, taken every quarter
// second while recording in practice mode, so it works like respawning at a checkpoint
// that is always right behind you: everything recorded after that point is dropped.
class FrameControls : public cocos2d::CCNode {
public:
    static FrameControls* create(PlayLayer* layer);
    // The controls of the running level, if any.
    static FrameControls* current();

    void toggleFreeze();
    void step(int ticks);
    void rewind();
    // Called after every game update; takes the rewind checkpoints.
    void afterUpdate();
    void onLevelReset();
    // Whether a touch lands on one of the buttons (so it must not count as a jump).
    bool hitsTouch(cocos2d::CCTouch* touch);

    void update(float dt) override;
    ~FrameControls() override;

protected:
    bool init(PlayLayer* layer);
    void refresh();

    PlayLayer* m_layer = nullptr;
    cocos2d::CCMenu* m_menu = nullptr;
    cocos2d::CCLabelBMFont* m_freezeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_tickLabel = nullptr;
    CCMenuItemSpriteExtra* m_rewindButton = nullptr;
    std::deque<std::pair<uint32_t, geode::Ref<CheckpointObject>>> m_history;
};
