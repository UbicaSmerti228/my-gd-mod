#pragma once

#include <Geode/Geode.hpp>

#include "../Bot.hpp"

class BotPopup : public geode::Popup {
public:
    static BotPopup* create();

protected:
    bool init();

    void buildModeTabs();
    void refreshInfo();
    void refreshList();
    void notify(std::string const& text, bool ok);

    cocos2d::CCMenu* m_tabMenu = nullptr;
    cocos2d::CCLabelBMFont* m_infoLabel = nullptr;
    cocos2d::CCLabelBMFont* m_speedLabel = nullptr;
    geode::TextInput* m_nameInput = nullptr;
    geode::ScrollLayer* m_list = nullptr;
};
