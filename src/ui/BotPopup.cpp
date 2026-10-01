#include "../Trace.hpp"
#include "BotPopup.hpp"
#include "../analysis/Analyzer.hpp"
#include "../analysis/LStar.hpp"
#include "../sim/RouteFinder.hpp"
#include "../stats/Forecast.hpp"

#include <Geode/ui/SliderNode.hpp>
#include <algorithm>
#include <fmt/format.h>

using namespace geode::prelude;

namespace {
    // GD's screen is 569 x 320 units; the popup has to leave room around it.
    constexpr float WIDTH = 420.f;
    constexpr float HEIGHT = 280.f;
    constexpr float LIST_WIDTH = 190.f;
    constexpr float LIST_HEIGHT = 82.f;
#ifdef GEODE_IS_MOBILE
    // Rows, icons and toggles sized for fingers.
    constexpr float ROW_HEIGHT = 32.f;
    constexpr float LOAD_ICON_SCALE = 0.42f;
    constexpr float DELETE_ICON_SCALE = 0.62f;
    constexpr float TOGGLE_SCALE = 0.72f;
#else
    constexpr float ROW_HEIGHT = 27.f;
    constexpr float LOAD_ICON_SCALE = 0.3f;
    constexpr float DELETE_ICON_SCALE = 0.5f;
    constexpr float TOGGLE_SCALE = 0.6f;
#endif
    constexpr float RIGHT_X = 312.f;

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
    this->setTitle("ILL Replay Bot", "goldFont.fnt", 0.75f, 16.f);

    auto help = CCMenuItemExt::createSpriteExtraWithFrameName("GJ_infoIcon_001.png", 0.7f, [this](CCMenuItemSpriteExtra*) {
        this->showHelp();
    });
    m_buttonMenu->addChildAtPosition(help, Anchor::TopRight, { -20.f, -20.f });

    m_replayPage = CCNode::create();
    m_replayPage->setContentSize({ WIDTH, HEIGHT });
    m_replayMenu = CCMenu::create();
    m_replayMenu->setContentSize({ WIDTH, HEIGHT });
    m_replayMenu->ignoreAnchorPointForPosition(false);
    m_replayPage->addChildAtPosition(m_replayMenu, Anchor::Center);
    m_mainLayer->addChild(m_replayPage);

    m_analysisPage = CCNode::create();
    m_analysisPage->setContentSize({ WIDTH, HEIGHT });
    m_analysisMenu = CCMenu::create();
    m_analysisMenu->setContentSize({ WIDTH, HEIGHT });
    m_analysisMenu->ignoreAnchorPointForPosition(false);
    m_analysisPage->addChildAtPosition(m_analysisMenu, Anchor::Center);
    m_mainLayer->addChild(m_analysisPage);

    this->buildReplayPage();
    this->buildAnalysisPage();

    m_pageTabs = CCMenu::create();
    m_pageTabs->setContentSize({ WIDTH, 24.f });
    m_pageTabs->ignoreAnchorPointForPosition(false);
    m_mainLayer->addChildAtPosition(m_pageTabs, Anchor::Top, { 0.f, -42.f });
    // Open on the analysis page while something runs there.
    this->showPage(SimController::active() ? 1 : 0);
    return true;
}

void BotPopup::buildPageTabs() {
    m_pageTabs->removeAllChildren();
    char const* names[] = { "Replay", "Analysis" };
    for (int i = 0; i < 2; ++i) {
        bool selected = m_page == i;
        auto sprite = ButtonSprite::create(names[i], 90, 0, 0.5f, true, "bigFont.fnt",
            selected ? "GJ_button_02.png" : "GJ_button_04.png", 22.f);
        if (!selected) sprite->setOpacity(170);
        auto button = CCMenuItemExt::createSpriteExtra(sprite, [this, i](CCMenuItemSpriteExtra*) {
            if (m_page == i) return;
            // The pressed tab is still running its callback, so rebuild next frame.
            queueInMainThread([self = Ref(this), i] { self->showPage(i); });
        });
        m_pageTabs->addChildAtPosition(button, Anchor::Center, { i == 0 ? -52.f : 52.f, 0.f });
    }
}

void BotPopup::showPage(int page) {
    m_page = page;
    m_replayPage->setVisible(page == 0);
    m_analysisPage->setVisible(page == 1);
    this->buildPageTabs();
    this->refreshInfo();
}

void BotPopup::addToggle(CCNode* page, CCMenu* menu, char const* label, char const* key, bool Bot::* field,
    CCPoint position, std::function<void()> onChange) {
    std::string savedKey = key;
    auto toggler = CCMenuItemExt::createTogglerWithStandardSprites(TOGGLE_SCALE, [this, field, savedKey, onChange](CCMenuItemToggler* toggler) {
        // The callback runs before the toggle flips its state.
        auto& bot = Bot::get();
        bot.*field = !toggler->isToggled();
        Mod::get()->setSavedValue(savedKey, bot.*field);
        if (onChange) onChange();
        this->refreshInfo();
    });
    toggler->toggle(Bot::get().*field);
    menu->addChildAtPosition(toggler, Anchor::BottomLeft, position);
    auto text = CCLabelBMFont::create(label, "bigFont.fnt");
    text->setScale(0.33f);
    text->setAnchorPoint({ 0.f, 0.5f });
    page->addChildAtPosition(text, Anchor::BottomLeft, position + CCPoint { 14.f, 0.f });
}

void BotPopup::buildReplayPage() {
    auto page = m_replayPage;
    auto menu = m_replayMenu;

    // Mode tabs and Start
    m_tabMenu = CCMenu::create();
    m_tabMenu->setContentSize({ WIDTH, 30.f });
    m_tabMenu->ignoreAnchorPointForPosition(false);
    page->addChildAtPosition(m_tabMenu, Anchor::Top, { 0.f, -78.f });
    this->buildModeTabs();

    auto start = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Start", 80, 0, 0.6f, true, "bigFont.fnt", "GJ_button_01.png", 28.f),
        [this](CCMenuItemSpriteExtra*) { this->resumeGame(true); }
    );
    menu->addChildAtPosition(start, Anchor::TopRight, { -60.f, -78.f });

    // Status
    page->addChildAtPosition(makePanel({ WIDTH - 30.f, 38.f }), Anchor::Top, { 0.f, -117.f });
    m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_infoLabel->setScale(0.62f);
    m_infoLabel->setAlignment(kCCTextAlignmentCenter);
    page->addChildAtPosition(m_infoLabel, Anchor::Top, { 0.f, -117.f });

    // Saved replays
    page->addChildAtPosition(makeCaption("Saved replays"), Anchor::BottomLeft, { 115.f, 132.f });
    page->addChildAtPosition(makePanel({ LIST_WIDTH + 10.f, LIST_HEIGHT + 8.f }), Anchor::BottomLeft, { 115.f, 78.f });
    m_list = ScrollLayer::create({ LIST_WIDTH, LIST_HEIGHT });
    m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(2.f));
    m_list->setPosition({ 115.f - LIST_WIDTH / 2.f, 78.f - LIST_HEIGHT / 2.f });
    page->addChild(m_list);

    // Save
    page->addChildAtPosition(makeCaption("Save current"), Anchor::BottomLeft, { RIGHT_X, 132.f });
    m_nameInput = TextInput::create(150.f, "replay name");
    m_nameInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.");
    m_nameInput->setMaxCharCount(64);
    m_nameInput->setScale(0.8f);
    auto const& bot = Bot::get();
    if (!bot.replayName.empty()) m_nameInput->setString(bot.replayName.c_str());
    else if (auto pl = PlayLayer::get(); pl && pl->m_level) m_nameInput->setString(pl->m_level->m_levelName.c_str());
    page->addChildAtPosition(m_nameInput, Anchor::BottomLeft, { RIGHT_X, 110.f });

    auto saveBtn = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Save", 80, 0, 0.6f, true, "bigFont.fnt", "GJ_button_01.png", 24.f),
        [this](CCMenuItemSpriteExtra*) {
            auto name = std::string(m_nameInput->getString().c_str());
            auto& bot = Bot::get();
            auto res = bot.save(name);
            if (!res) return this->notify(res.unwrapErr(), false);
            bot.markSaved(name);
            this->notify(fmt::format("Saved \"{}\"", name), true);
            this->refreshList();
            this->refreshInfo();
        }
    );
    menu->addChildAtPosition(saveBtn, Anchor::BottomLeft, { RIGHT_X, 84.f });

    // Speed
    m_speedLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_speedLabel->setScale(0.4f);
    page->addChildAtPosition(m_speedLabel, Anchor::BottomLeft, { RIGHT_X, 60.f });
    auto slider = SliderNode::create([this](SliderNode*, float value) {
        Bot::get().setSpeed(value);
        m_speedLabel->setString(fmt::format("Speed x{:.2f}", Bot::get().speed).c_str());
    });
    slider->setMin(0.05f);
    slider->setMax(2.f);
    slider->setSnapStep(0.05f);
    slider->setValue(Bot::get().speed);
    slider->setScale(0.7f);
    page->addChildAtPosition(slider, Anchor::BottomLeft, { RIGHT_X, 42.f });
    m_speedLabel->setString(fmt::format("Speed x{:.2f}", Bot::get().speed).c_str());

    // Bottom row
    this->addToggle(page, menu, "Label", "show-overlay", &Bot::showOverlay, { 14.f, 16.f });
    this->addToggle(page, menu, "CBF", "cbf-mode", &Bot::cbfMode, { 84.f, 16.f });
    this->addToggle(page, menu, "Clicks", "clickbot", &Bot::clickbot, { 144.f, 16.f });
    this->addToggle(page, menu, "Controls", "frame-controls", &Bot::frameControls, { 222.f, 16.f });

    auto exportBtn = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Export", 56, 0, 0.4f, true, "bigFont.fnt", "GJ_button_04.png", 22.f),
        [this](CCMenuItemSpriteExtra*) {
            auto name = std::string(m_nameInput->getString().c_str());
            auto res = Bot::get().exportGdr(name);
            if (!res) return this->notify(res.unwrapErr(), false);
            auto rounded = res.unwrap();
            this->notify(rounded
                ? fmt::format("Exported \"{}.gdr2\" ({} CBF clicks rounded to a tick)", name, rounded)
                : fmt::format("Exported \"{}.gdr2\" for other bots", name), true);
            this->refreshList();
        }
    );
    menu->addChildAtPosition(exportBtn, Anchor::BottomRight, { -88.f, 16.f });

    auto folderBtn = CCMenuItemExt::createSpriteExtra(
        ButtonSprite::create("Folder", 48, 0, 0.4f, true, "bigFont.fnt", "GJ_button_04.png", 22.f),
        [](CCMenuItemSpriteExtra*) {
            (void)file::createDirectoryAll(Bot::replayDir());
#ifdef GEODE_IS_MOBILE
            // No file manager to open from inside the game on phones: show where it is.
            FLAlertLayer::create(nullptr, "Replay folder",
                fmt::format("Replays (.ilr) and .gdr2 files from other bots go here:\n<cy>{}</c>",
                    utils::string::pathToString(Bot::replayDir())),
                "OK", nullptr, 380.f)->show();
#else
            file::openFolder(Bot::replayDir());
#endif
        }
    );
    menu->addChildAtPosition(folderBtn, Anchor::BottomRight, { -32.f, 16.f });

    this->refreshList();
}

void BotPopup::buildAnalysisPage() {
    auto page = m_analysisPage;
    auto menu = m_analysisMenu;

    page->addChildAtPosition(makePanel({ WIDTH - 30.f, 56.f }), Anchor::Top, { 0.f, -95.f });
    m_analysisInfo = CCLabelBMFont::create("", "chatFont.fnt");
    m_analysisInfo->setScale(0.6f);
    m_analysisInfo->setAlignment(kCCTextAlignmentCenter);
    page->addChildAtPosition(m_analysisInfo, Anchor::Top, { 0.f, -95.f });

    struct Action { char const* label; char const* texture; int kind; char const* hint; };
    Action actions[] = {
        { "Analyze", "GJ_button_03.png", 0, "Frame window\nof every click" },
        { "Optimize", "GJ_button_02.png", 1, "Move clicks to the\nmiddle of their windows" },
        { "Find route", "GJ_button_05.png", 2, "Beat the level\nwithout a replay" },
    };
    auto running = SimController::active();
    float x = 75.f;
    for (auto const& action : actions) {
        bool isRunning = running && (
            (action.kind == 2 && running == &RouteFinder::get()) ||
            (action.kind != 2 && running == &Analyzer::get())
        );
        auto kind = action.kind;
        auto button = CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create(
                isRunning ? "Stop" : action.label, 110, 0, 0.5f, true, "bigFont.fnt",
                isRunning ? "GJ_button_06.png" : action.texture, 28.f
            ),
            [this, kind](CCMenuItemSpriteExtra*) {
                ILL_TRACE("button {} pressed", kind);
                if (auto sim = SimController::active()) {
                    sim->cancel(PlayLayer::get());
                    return this->onClose(nullptr);
                }
                ILL_TRACE("button {}: starting", kind);
                auto res = kind == 2 ? RouteFinder::get().start(PlayLayer::get())
                    : Analyzer::get().start(PlayLayer::get(), kind == 1);
                ILL_TRACE("button {}: started ({})", kind, res ? "ok" : "error");
                if (!res) return this->notify(res.unwrapErr(), false);
                // It runs in the level: unpause straight away. Progress is shown bottom right.
                this->resumeGame(false);
            }
        );
        menu->addChildAtPosition(button, Anchor::BottomLeft, { x, 138.f });
        auto hint = CCLabelBMFont::create(action.hint, "chatFont.fnt");
        hint->setScale(0.5f);
        hint->setAlignment(kCCTextAlignmentCenter);
        hint->setOpacity(200);
        page->addChildAtPosition(hint, Anchor::BottomLeft, { x, 108.f });
        x += 135.f;
    }

    page->addChildAtPosition(makeCaption("Show during playback"), Anchor::BottomLeft, { WIDTH / 2.f, 76.f });
    this->addToggle(page, menu, "Counter", "show-counter", &Bot::showCounter, { 22.f, 50.f });
    this->addToggle(page, menu, "Paths", "show-paths", &Bot::showPaths, { 122.f, 50.f });
    this->addToggle(page, menu, "Sounds", "play-sounds", &Bot::playSounds, { 212.f, 50.f });
    // Whole-tick and CBF windows give different L* values.
    this->addToggle(page, menu, "CBF windows", "use-cbf", &Bot::useCbf, { 292.f, 50.f }, [] { LStar::computeAsync(); });

    auto note = CCLabelBMFont::create("Counter, paths and L* appear in Play mode after an analysis.", "chatFont.fnt");
    note->setScale(0.5f);
    note->setOpacity(170);
    page->addChildAtPosition(note, Anchor::Bottom, { 0.f, 20.f });
}

void BotPopup::buildModeTabs() {
    m_tabMenu->removeAllChildren();

    struct Tab { char const* name; BotMode mode; char const* texture; };
    Tab tabs[] = {
        { "Off", BotMode::Off, "GJ_button_05.png" },
        { "Record", BotMode::Record, "GJ_button_06.png" },
        { "Play", BotMode::Play, "GJ_button_01.png" },
    };

    float x = 60.f;
    for (auto const& tab : tabs) {
        bool selected = Bot::get().mode == tab.mode;
        auto sprite = ButtonSprite::create(
            tab.name, 80, 0, 0.55f, true, "bigFont.fnt",
            selected ? tab.texture : "GJ_button_04.png", 28.f
        );
        if (!selected) sprite->setOpacity(160);
        auto mode = tab.mode;
        auto button = CCMenuItemExt::createSpriteExtra(sprite, [this, mode](CCMenuItemSpriteExtra*) {
            auto& bot = Bot::get();
            ILL_TRACE("mode tab {} pressed", static_cast<int>(mode));
            if (bot.mode == mode || SimController::active()) return;

            auto apply = [this, mode] {
                auto& bot = Bot::get();
                bot.setMode(mode);
                // The pressed tab is still running its callback, so rebuild the tabs next frame.
                queueInMainThread([self = Ref(this)] { self->buildModeTabs(); });
                this->refreshInfo();
                if (mode == BotMode::Play && bot.replay.levelID && currentLevelID() && bot.replay.levelID != currentLevelID()) {
                    this->notify("This replay was recorded on another level", false);
                }
                else if (mode == BotMode::Play && bot.replay.tps && bot.measuredTps && bot.replay.tps != bot.measuredTps) {
                    this->notify(fmt::format("Recorded at {} TPS, running at {} TPS", bot.replay.tps, bot.measuredTps), false);
                }
            };

            if (mode == BotMode::Record && bot.unsaved && !bot.replay.inputs.empty()) {
                createQuickPopup(
                    "Unsaved replay",
                    "The current replay is <cr>not saved</c> and will be replaced by the new recording. Continue?",
                    "Cancel", "Record",
                    [apply](FLAlertLayer*, bool yes) { if (yes) apply(); }
                );
                return;
            }
            apply();
        });
        m_tabMenu->addChildAtPosition(button, Anchor::Left, { x, 0.f });
        x += 90.f;
    }
}

void BotPopup::refreshInfo() {
    auto& bot = Bot::get();
    auto const& replay = bot.replay;

    char const* modeName = bot.mode == BotMode::Record ? "Recording"
        : bot.mode == BotMode::Play ? "Playback" : "Off";
    auto name = !bot.replayName.empty() ? bot.replayName
        : !replay.levelName.empty() ? replay.levelName : std::string("no replay");
    auto next = bot.mode == BotMode::Off ? std::string("pick Record or Play, then Start")
        : std::string("press Start to restart the level");

    if (m_infoLabel) {
        m_infoLabel->setString(fmt::format(
            "{}: {}{}   |   {}\n{} inputs   |   {} TPS{}",
            modeName, name, bot.unsaved ? " (unsaved)" : "", next,
            replay.inputs.size(), replay.effectiveTps(), bot.cbfMode ? "   |   CBF mode" : ""
        ).c_str());
    }

    if (!m_analysisInfo) return;
    std::string status;
    if (auto sim = SimController::active()) {
        status = sim->statusText();
    }
    else if (replay.hasAnalysis()) {
        size_t counted = 0, unreliable = 0;
        for (auto const& a : replay.analysis) {
            counted += a.analyzed && !a.unreliable ? 1 : 0;
            unreliable += a.unreliable ? 1 : 0;
        }
        status = fmt::format("{} clicks measured, {} could not be reproduced", counted, unreliable);
    }
    else {
        status = replay.inputs.empty() ? "Record or load a replay to analyze it" : "Not analyzed yet";
    }

    auto lstar = !replay.lstar.empty() ? fmt::format("Level L* {:.2f}{}", replay.lstar.back(), bot.useCbf ? " (CBF)" : "")
        : LStar::isComputing() ? std::string("L* computing...") : std::string("L* -- (needs an analysis)");

    auto const& estimate = Forecast::estimate();
    auto forecast = estimate.valid ? fmt::format("Your L {:.1f} from {} attempts", estimate.precision, estimate.attempts)
        : fmt::format("Your L: play the level ({} attempts, {} deaths logged)", estimate.attempts, estimate.deaths);

    m_analysisInfo->setString(fmt::format("{}\n{}\n{}", status, lstar, forecast).c_str());
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

    auto const& current = Bot::get().replayName;
    for (auto const& name : names) {
        auto row = CCMenu::create();
        row->ignoreAnchorPointForPosition(false);
        row->setContentSize({ LIST_WIDTH, ROW_HEIGHT });

        bool loaded = name == current;
        auto bg = makePanel({ LIST_WIDTH - 4.f, ROW_HEIGHT - 2.f });
        bg->setOpacity(loaded ? 140 : 60);
        if (loaded) bg->setColor({ 40, 120, 60 });
        row->addChildAtPosition(bg, Anchor::Center);

        auto label = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 0.5f });
        label->limitLabelWidth(120.f, 0.35f, 0.1f);
        if (loaded) label->setColor({ 140, 255, 160 });
        row->addChildAtPosition(label, Anchor::Left, { 6.f, 5.f });

        // What is in the file: clicks, analysis, CBF, and whether it is for this level.
        std::string details;
        if (auto info = Bot::readReplay(name)) {
            auto const& r = info.unwrap();
            size_t presses = std::count_if(r.inputs.begin(), r.inputs.end(), [](auto const& i) { return i.down; });
            bool cbf = std::any_of(r.inputs.begin(), r.inputs.end(), [](auto const& i) { return i.subtick > 0.f; });
            details = fmt::format("{} clicks", presses);
            if (r.hasAnalysis()) details += "  |  analyzed";
            if (cbf) details += "  |  CBF";
            if (r.levelID && currentLevelID() && r.levelID != currentLevelID()) details += "  |  other level";
        }
        else {
            details = "unreadable file";
        }
        auto detailLabel = CCLabelBMFont::create(details.c_str(), "chatFont.fnt");
        detailLabel->setAnchorPoint({ 0.f, 0.5f });
        detailLabel->limitLabelWidth(125.f, 0.42f, 0.1f);
        detailLabel->setColor({ 190, 190, 190 });
        row->addChildAtPosition(detailLabel, Anchor::Left, { 6.f, -6.f });

        auto loadBtn = CCMenuItemExt::createSpriteExtraWithFrameName(
            "GJ_playBtn2_001.png", LOAD_ICON_SCALE,
            [this, name](CCMenuItemSpriteExtra*) {
                auto& bot = Bot::get();
                auto res = bot.load(name);
                if (!res) return this->notify(res.unwrapErr(), false);
                if (bot.mode == BotMode::Record) bot.setMode(BotMode::Off);
                // A .gdr2 from another bot is saved back as this bot's own file.
                auto plain = name.ends_with(".gdr2") ? name.substr(0, name.size() - 5) : name;
                m_nameInput->setString(plain.c_str());
                this->buildModeTabs();
                this->refreshInfo();
                // Rebuilding the list removes this button, so do it next frame.
                queueInMainThread([self = Ref(this)] { self->refreshList(); });
                this->notify(fmt::format("Loaded \"{}\"", name), true);
            }
        );
        row->addChildAtPosition(loadBtn, Anchor::Right, { -44.f, 0.f });

        auto deleteBtn = CCMenuItemExt::createSpriteExtraWithFrameName(
            "GJ_deleteIcon_001.png", DELETE_ICON_SCALE,
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

void BotPopup::resumeGame(bool restart) {
    PauseLayer* pause = nullptr;
    if (auto scene = CCDirector::get()->getRunningScene()) pause = scene->getChildByType<PauseLayer>(0);
    Ref<PauseLayer> keep = pause;
    ILL_TRACE("resume: pause layer {}", static_cast<void*>(pause));
    this->onClose(nullptr);
    ILL_TRACE("resume: popup closed");
    if (!pause) return;
    if (restart) pause->onRestartFull(nullptr);
    else pause->onResume(nullptr);
    ILL_TRACE("resume: done");
}

void BotPopup::showHelp() {
    FLAlertLayer::create(
        nullptr, "How to use",
        "<cg>Record</c>: pick Record, press Start, play the level (practice and start positions work), "
        "then Save.\n"
        "<cg>Showcase</c>: load a replay, pick Play, press Start.\n"
        "<cg>CBF mode</c>: turn on if you play with Click Between Frames.\n"
        "<cg>Frame controls</c> (right side, or F / V / C): freeze, one tick, rewind while recording.\n"
        "<cg>.gdr2</c> replays from other bots in the replay folder show up in the list; Export writes one.\n"
        "<cy>Analysis</c> page: Analyze measures every click's frame window and L*; "
        "Optimize makes the replay safer; Find route beats the level with no replay.\n"
        "While the bot is on, nothing is saved to your progress.",
        "OK", nullptr, 420.f
    )->show();
}
