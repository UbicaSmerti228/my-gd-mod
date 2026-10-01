#pragma once

#include <Geode/Geode.hpp>

#include "../Bot.hpp"

#include <functional>

// The bot menu, opened from the pause menu. Two pages:
// - Replay: mode (Off / Record / Play) with a Start button, the replay list, saving,
//   playback speed, CBF mode.
// - Analysis: frame window analysis, optimizer, route finder, and what the HUD shows.
class BotPopup : public geode::Popup {
public:
    static BotPopup* create();

protected:
    bool init();

    void buildPageTabs();
    void showPage(int page);
    void buildReplayPage();
    void buildAnalysisPage();
    void buildModeTabs();
    void refreshInfo();
    void refreshList();
    void notify(std::string const& text, bool ok);
    void addToggle(cocos2d::CCNode* page, cocos2d::CCMenu* menu, char const* label, char const* key,
        bool Bot::* field, cocos2d::CCPoint position, std::function<void()> onChange = nullptr);
    // Closes the popup and the pause menu; restarts the level from the start if asked.
    void resumeGame(bool restart);
    void showHelp();

    int m_page = 0;
    cocos2d::CCMenu* m_pageTabs = nullptr;
    cocos2d::CCNode* m_replayPage = nullptr;
    cocos2d::CCMenu* m_replayMenu = nullptr;
    cocos2d::CCNode* m_analysisPage = nullptr;
    cocos2d::CCMenu* m_analysisMenu = nullptr;

    cocos2d::CCMenu* m_tabMenu = nullptr;
    cocos2d::CCLabelBMFont* m_infoLabel = nullptr;
    cocos2d::CCLabelBMFont* m_analysisInfo = nullptr;
    cocos2d::CCLabelBMFont* m_speedLabel = nullptr;
    geode::TextInput* m_nameInput = nullptr;
    geode::ScrollLayer* m_list = nullptr;
};
