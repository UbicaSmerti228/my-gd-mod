#pragma once

#include "../Bot.hpp"

#include <Geode/Geode.hpp>

#include <array>
#include <deque>

// Frame window counter in the style of NaN's showcases: per-category counts in the
// top left, ring markers with the window size at every click, L* in the bottom left,
// a CPS counter in the top right, and the past / future / alternative trajectories.
class FrameHud : public cocos2d::CCNode {
public:
    static constexpr int CATEGORY_COUNT = 7;

    static FrameHud* create(PlayLayer* layer);

    // Category index for a window (0 = "9-12" ... 6 = "1"), -1 when it is not counted.
    static int categoryFor(InputAnalysis const& analysis);
    static cocos2d::ccColor3B categoryColor(int category);

    void onInputPlayed(size_t index);
    void resetTo(uint32_t tick);
    void update(float dt) override;

protected:
    bool init(PlayLayer* layer);

    void buildCounter();
    void refreshCounts(bool animate, int changedCategory);
    void refreshPrecision();
    void spawnMarker(size_t index, int category);
    void drawTrajectories();
    bool active() const;

    PlayLayer* m_layer = nullptr;
    cocos2d::CCNode* m_counter = nullptr;
    std::array<cocos2d::CCLabelBMFont*, CATEGORY_COUNT> m_countLabels {};
    std::array<int, CATEGORY_COUNT> m_counts {};
    cocos2d::CCLabelBMFont* m_shareLabel = nullptr;
    cocos2d::CCLabelBMFont* m_lstarLabel = nullptr;
    cocos2d::CCLabelBMFont* m_cpsLabel = nullptr;
    geode::Ref<cocos2d::CCDrawNode> m_draw;
    geode::Ref<cocos2d::CCNode> m_markers;

    long m_lastIndex = -1;
    std::deque<double> m_pressTimes;
    int m_maxCps = 0;
    int m_totalPresses = 0;
};
