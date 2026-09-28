#include "BotPopup.hpp"

#include <fmt/format.h>

using namespace geode::prelude;

namespace {
    constexpr float WIDTH = 400.f;
    constexpr float HEIGHT = 280.f;
    constexpr float LIST_WIDTH = 180.f;
    constexpr float LIST_HEIGHT = 108.f;

    NineSlice* makePanel(CCSize size) {
        auto panel = NineSlice::createWithSpriteFrameName("square02_small.png");
        panel->setContentSize(size);
        panel->setColor({ 0, 0, 0 });
        panel->setOpacity(90);
        return panel;
    }

    CCLabelBMFont* makeCaption(char const* text) {
        auto label = CCLabelBMFont::create(text, "goldFont.fnt");
        label->setScale(0.5f);
        return label;
    }

    int currentLevelID() {
        auto pl = PlayLayer::get();
        return pl && pl->m_level ? pl->m_level->m_levelID.value() : 0;
    }
}

BotPopup* BotPopup::create() {
    auto ret = new BotPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool BotPopup::init() {
    if (!Popup::init(WIDTH, HEIGHT, "GJ_square02.png")) return false;
    this->setTitle("ILL Replay Bot", "goldFont.fnt", 0.8f, 18.f);

    // Mode tabs
    m_tabMenu = CCMenu::create();
    m_tabMenu->setContentSize({ WIDTH, 30.f });
    m_tabMenu->ignoreAnchorPointForPosition(false);
    m_mainLayer->addChildAtPosition(m_tabMenu, Anchor::Top, { 0.f, -52.f });
    this->buildModeTabs();

    // Status panel
    auto infoPanel = makePanel({ WIDTH - 30.f, 36.f });
    m_mainLayer->addChildAtPosition(infoPanel, Anchor::Top, { 0.f, -92.f });
    m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_infoLabel->setScale(0.7f);
    m_infoLabel->setAlignment(kCCTextAlignmentCenter);
    m_mainLayer->addChildAtPosition(m_infoLabel, Anchor::Top, { 0.f, -92.f });

    // Left column: saved replays
    m_mainLayer->addChildAtPosition(makeCaption("Saved replays"), Anchor::BottomLeft, { 110.f, 150.f });
    auto listPanel = makePanel({ LIST_WIDTH + 10.f, LIST_HEIGHT + 8.f });
    m_mainLayer->addChildAtPosition(listPanel, Anchor::BottomLeft, { 110.f, 82.f });
    m_list = ScrollLayer::create({ LIST_WIDTH, LIST_HEIGHT });
    m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(2.f));
    m_list->setPosition({ 110.f - LIST_WIDTH / 2.f, 82.f - LIST_HEIGHT / 2.f });
    m_mainLayer->addChild(m_list);

    // Right column: save, speed, overlay
    auto rightX = 300.f;
    m_mainLayer->addChildAtPosition(makeCaption("Save current"), Anchor::BottomLeft, { rightX, 150.f });

    m_nameInput = TextInput::create(150.f, "replay name");
    m_nameInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.");
    m_nameInput->setMaxCharCount(64);
    m_nameInput->setScale(0.85f);
    if (auto pl = PlayLayer::get(); pl && pl->m_level) {
        m_nameInput->setString(pl->m_level->m_levelName.c_str());
    }
    m_mainLayer->addChildAtPosition(m_nameInput, Anchor::BottomLeft, { rightX, 126.f });

    auto saveBtn = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Save", 80, 0, 0.7f, true, "bigFont.fnt", "GJ_button_01.png", 26.f),
        [this](CCMenuItemSpriteExtra*) {
            auto name = std::string(m_nameInput->getString().c_str());
            auto res = Bot::get().save(name);
            if (!res) return this->notify(res.unwrapErr(), false);
            this->notify(fmt::format("Saved \"{}\"", name), true);
            this->refreshList();
        }
    );
    m_buttonMenu->addChildAtPosition(saveBtn, Anchor::BottomLeft, { rightX, 98.f });

    m_speedLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_speedLabel->setScale(0.4f);
    m_mainLayer->addChildAtPosition(m_speedLabel, Anchor::BottomLeft, { rightX, 72.f });

    auto slider = SliderNode::create([this](SliderNode*, float value) {
        Bot::get().setSpeed(value);
        m_speedLabel->setString(fmt::format("Speed x{:.2f}", Bot::get().speed).c_str());
    });
    slider->setMin(0.05f);
    slider->setMax(2.f);
    slider->setSnapStep(0.05f);
    slider->setValue(Bot::get().speed);
    slider->setScale(0.7f);
    m_mainLayer->addChildAtPosition(slider, Anchor::BottomLeft, { rightX, 54.f });
    m_speedLabel->setString(fmt::format("Speed x{:.2f}", Bot::get().speed).c_str());

    // Bottom row
    auto overlayToggle = CCMenuItemExt::createTogglerWithStandardSprites(0.6f, [](CCMenuItemToggler* toggler) {
        // The callback runs before the toggle flips its state.
        auto& bot = Bot::get();
        bot.showOverlay = !toggler->isToggled();
        Mod::get()->setSavedValue("show-overlay", bot.showOverlay);
    });
    overlayToggle->toggle(Bot::get().showOverlay);
    m_buttonMenu->addChildAtPosition(overlayToggle, Anchor::BottomLeft, { 30.f, 18.f });
    auto overlayLabel = CCLabelBMFont::create("TPS / bot label on screen", "bigFont.fnt");
    overlayLabel->setScale(0.3f);
    overlayLabel->setAnchorPoint({ 0.f, 0.5f });
    m_mainLayer->addChildAtPosition(overlayLabel, Anchor::BottomLeft, { 45.f, 18.f });

    auto folderBtn = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Folder", 70, 0, 0.5f, true, "bigFont.fnt", "GJ_button_04.png", 22.f),
        [](CCMenuItemSpriteExtra*) {
            (void)file::createDirectoryAll(Bot::replayDir());
            file::openFolder(Bot::replayDir());
        }
    );
    m_buttonMenu->addChildAtPosition(folderBtn, Anchor::BottomRight, { -50.f, 18.f });

    this->refreshInfo();
    this->refreshList();
    return true;
}

void BotPopup::buildModeTabs() {
    m_tabMenu->removeAllChildren();

    struct Tab { char const* name; BotMode mode; char const* texture; };
    Tab tabs[] = {
        { "Off", BotMode::Off, "GJ_button_05.png" },
        { "Record", BotMode::Record, "GJ_button_06.png" },
        { "Play", BotMode::Play, "GJ_button_01.png" },
    };

    float x = WIDTH / 2.f - 100.f;
    for (auto const& tab : tabs) {
        bool selected = Bot::get().mode == tab.mode;
        auto sprite = ButtonSprite::create(
            tab.name, 90, 0, 0.6f, true, "bigFont.fnt",
            selected ? tab.texture : "GJ_button_04.png", 28.f
        );
        if (!selected) sprite->setOpacity(160);
        auto mode = tab.mode;
        auto button = CCMenuItemExt::createSpriteExtra(sprite, [this, mode](CCMenuItemSpriteExtra*) {
            auto& bot = Bot::get();
            if (bot.mode == mode) return;

            auto apply = [this, mode] {
                auto& bot = Bot::get();
                bot.setMode(mode);
                // The pressed tab is still running its callback, so rebuild the tabs next frame.
                queueInMainThread([self = Ref(this)] { self->buildModeTabs(); });
                this->refreshInfo();
                if (mode == BotMode::Play) {
                    if (bot.replay.levelID && currentLevelID() && bot.replay.levelID != currentLevelID()) {
                        this->notify("This replay was recorded on another level", false);
                    }
                    else if (bot.replay.tps && bot.measuredTps && bot.replay.tps != bot.measuredTps) {
                        this->notify(fmt::format("Recorded at {} TPS, running at {} TPS", bot.replay.tps, bot.measuredTps), false);
                    }
                    else {
                        this->notify("Restart the level to start playback", true);
                    }
                }
                else if (mode == BotMode::Record) {
                    this->notify("Restart the level to start recording", true);
                }
            };

            if (mode == BotMode::Record && !bot.replay.inputs.empty()) {
                createQuickPopup(
                    "New recording",
                    "The current replay will be <cr>discarded</c> unless you saved it. Continue?",
                    "Cancel", "Record",
                    [apply](FLAlertLayer*, bool yes) { if (yes) apply(); }
                );
                return;
            }
            apply();
        });
        m_tabMenu->addChildAtPosition(button, Anchor::Left, { x, 0.f });
        x += 100.f;
    }
}

void BotPopup::refreshInfo() {
    auto& bot = Bot::get();
    auto const& replay = bot.replay;

    char const* modeName = bot.mode == BotMode::Record ? "Recording"
        : bot.mode == BotMode::Play ? "Playback" : "Off";
    auto level = replay.levelName.empty() ? std::string("no replay loaded") : replay.levelName;
    auto tps = replay.tps ? fmt::format("{} TPS", replay.tps) : std::string("TPS not measured yet");

    m_infoLabel->setString(fmt::format(
        "Mode: {}   |   {}\n{} inputs   |   {} ticks   |   {}",
        modeName, level, replay.inputs.size(), replay.lastTick(), tps
    ).c_str());
}

void BotPopup::refreshList() {
    auto content = m_list->m_contentLayer;
    content->removeAllChildren();

    auto names = Bot::listReplays();
    if (names.empty()) {
        auto empty = CCLabelBMFont::create("No replays yet", "chatFont.fnt");
        empty->setScale(0.6f);
        empty->setOpacity(150);
        content->addChild(empty);
    }

    for (auto const& name : names) {
        auto row = CCMenu::create();
        row->ignoreAnchorPointForPosition(false);
        row->setContentSize({ LIST_WIDTH, 22.f });

        auto bg = makePanel({ LIST_WIDTH - 4.f, 20.f });
        bg->setOpacity(60);
        row->addChildAtPosition(bg, Anchor::Center);

        auto label = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 0.5f });
        label->limitLabelWidth(110.f, 0.35f, 0.1f);
        row->addChildAtPosition(label, Anchor::Left, { 6.f, 0.f });

        auto loadBtn = CCMenuItemExt::createSpriteExtraWithFrameName(
            "GJ_playBtn2_001.png", 0.3f,
            [this, name](CCMenuItemSpriteExtra*) {
                auto& bot = Bot::get();
                auto res = bot.load(name);
                if (!res) return this->notify(res.unwrapErr(), false);
                if (bot.mode == BotMode::Record) bot.setMode(BotMode::Off);
                this->buildModeTabs();
                this->refreshInfo();
                this->notify(fmt::format("Loaded \"{}\"", name), true);
            }
        );
        row->addChildAtPosition(loadBtn, Anchor::Right, { -40.f, 0.f });

        auto deleteBtn = CCMenuItemExt::createSpriteExtraWithFrameName(
            "GJ_deleteIcon_001.png", 0.5f,
            [this, name](CCMenuItemSpriteExtra*) {
                createQuickPopup(
                    "Delete replay",
                    fmt::format("Delete <cy>{}</c>? This cannot be undone.", name),
                    "Cancel", "Delete",
                    [this, name](FLAlertLayer*, bool yes) {
                        if (!yes) return;
                        auto res = Bot::remove(name);
                        if (!res) return this->notify(res.unwrapErr(), false);
                        this->refreshList();
                    }
                );
            }
        );
        row->addChildAtPosition(deleteBtn, Anchor::Right, { -14.f, 0.f });

        content->addChild(row);
    }

    content->updateLayout();
    m_list->scrollToTop();
}

void BotPopup::notify(std::string const& text, bool ok) {
    Notification::create(text, ok ? NotificationIcon::Success : NotificationIcon::Warning)->show();
}
