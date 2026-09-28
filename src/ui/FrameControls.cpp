#include "FrameControls.hpp"
#include "../Bot.hpp"
#include "../sim/SimController.hpp"

using namespace geode::prelude;

namespace {
    FrameControls* s_current = nullptr;

    // A rewind point every quarter second, the last six seconds kept.
    constexpr uint32_t HISTORY_INTERVAL = 60;
    constexpr size_t HISTORY_SIZE = 24;
    constexpr uint32_t REWIND_TICKS = 240;

#ifdef GEODE_IS_MOBILE
    // Big enough for a thumb.
    constexpr float BUTTON_SCALE = 0.9f;
    constexpr float BUTTON_GAP = 48.f;
#else
    constexpr float BUTTON_SCALE = 0.65f;
    constexpr float BUTTON_GAP = 36.f;
#endif

    CCMenuItemSpriteExtra* makeButton(char const* text, CCLabelBMFont** label, geode::Function<void(CCMenuItemSpriteExtra*)> callback) {
        auto top = CCLabelBMFont::create(text, "bigFont.fnt");
        if (label) *label = top;
        auto sprite = CircleButtonSprite::create(top, CircleBaseColor::DarkPurple, CircleBaseSize::Small);
        sprite->setScale(BUTTON_SCALE);
        return CCMenuItemExt::createSpriteExtra(sprite, std::move(callback));
    }
}

FrameControls* FrameControls::create(PlayLayer* layer) {
    auto ret = new FrameControls();
    if (ret->init(layer)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

FrameControls* FrameControls::current() {
    return s_current;
}

FrameControls::~FrameControls() {
    if (s_current == this) s_current = nullptr;
}

bool FrameControls::init(PlayLayer* layer) {
    if (!CCNode::init()) return false;
    m_layer = layer;
    s_current = this;
    this->setID("frame-controls"_spr);

    auto winSize = CCDirector::get()->getWinSize();
    m_menu = CCMenu::create();
    m_menu->setPosition({ 0.f, 0.f });
    this->addChild(m_menu);

    float x = winSize.width - 28.f;
    float y = winSize.height / 2.f + BUTTON_GAP;

    auto freeze = makeButton("||", &m_freezeLabel, [this](CCMenuItemSpriteExtra*) { this->toggleFreeze(); });
    freeze->setPosition({ x, y });
    m_menu->addChild(freeze);

    auto step = makeButton("+1", nullptr, [this](CCMenuItemSpriteExtra*) { this->step(1); });
    step->setPosition({ x, y - BUTTON_GAP });
    m_menu->addChild(step);

    m_rewindButton = makeButton("<<", nullptr, [this](CCMenuItemSpriteExtra*) { this->rewind(); });
    m_rewindButton->setPosition({ x, y - 2.f * BUTTON_GAP });
    m_menu->addChild(m_rewindButton);

    m_tickLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_tickLabel->setScale(0.3f);
    m_tickLabel->setAnchorPoint({ 1.f, 0.5f });
    m_tickLabel->setPosition({ winSize.width - 6.f, y - 2.8f * BUTTON_GAP });
    this->addChild(m_tickLabel);

    this->scheduleUpdate();
    this->refresh();
    return true;
}

void FrameControls::update(float) {
    this->refresh();
}

void FrameControls::refresh() {
    auto& bot = Bot::get();
    bool visible = bot.frameControls && bot.mode != BotMode::Off && !SimController::active();
    this->setVisible(visible);
    if (!visible) return;
    m_rewindButton->setVisible(bot.mode == BotMode::Record);
    m_freezeLabel->setString(bot.frozen ? ">" : "||");
    m_tickLabel->setString(bot.frozen ? fmt::format("tick {}", bot.tick).c_str() : "");
}

void FrameControls::toggleFreeze() {
    auto& bot = Bot::get();
    bot.frozen = !bot.frozen;
    bot.pendingSteps = 0;
    this->refresh();
}

void FrameControls::step(int ticks) {
    auto& bot = Bot::get();
    bot.frozen = true;
    bot.pendingSteps += ticks;
    this->refresh();
}

void FrameControls::rewind() {
    auto& bot = Bot::get();
    if (bot.mode != BotMode::Record) return;
    if (!m_layer->m_isPracticeMode) {
        // Rewinding restarts at a checkpoint, which the game only allows in practice.
        m_layer->togglePracticeMode(true);
        Notification::create("Practice mode on: rewind points are kept from now on", NotificationIcon::Info)->show();
        return;
    }
    if (m_history.empty()) {
        Notification::create("Nothing to rewind to yet", NotificationIcon::Warning)->show();
        return;
    }

    uint32_t target = bot.tick > REWIND_TICKS ? bot.tick - REWIND_TICKS : 0;
    size_t pick = 0;
    for (size_t i = 0; i < m_history.size(); ++i) {
        if (m_history[i].first <= target) pick = i;
    }
    auto checkpoint = m_history[pick].second;
    m_history.erase(m_history.begin() + static_cast<long>(pick) + 1, m_history.end());

    // Respawn at our checkpoint without touching the player's own practice checkpoints.
    auto checkpoints = m_layer->m_checkpointArray;
    checkpoints->addObject(checkpoint);
    m_layer->resetLevel();
    checkpoints->removeObject(checkpoint);
}

void FrameControls::afterUpdate() {
    auto& bot = Bot::get();
    if (bot.mode != BotMode::Record || !m_layer->m_isPracticeMode || SimController::active()) return;
    if (m_layer->m_playerDied || m_layer->m_levelEndAnimationStarted) return;
    if (!m_history.empty() && bot.tick < m_history.back().first + HISTORY_INTERVAL) return;
    if (!m_history.empty() && bot.tick == m_history.back().first) return;
    if (auto checkpoint = m_layer->createCheckpoint()) {
        m_history.push_back({ bot.tick, checkpoint });
        if (m_history.size() > HISTORY_SIZE) m_history.pop_front();
    }
}

void FrameControls::onLevelReset() {
    // After a respawn, points later than where we are now belong to the old attempt.
    auto tick = Bot::get().tick;
    while (!m_history.empty() && m_history.back().first > tick) m_history.pop_back();
}

bool FrameControls::hitsTouch(CCTouch* touch) const {
    if (!this->isVisible() || !touch) return false;
    auto location = touch->getLocation();
    for (auto item : CCArrayExt<CCNode*>(m_menu->getChildren())) {
        if (!item->isVisible()) continue;
        auto local = item->convertToNodeSpace(location);
        auto size = item->getContentSize();
        if (local.x >= 0.f && local.y >= 0.f && local.x <= size.width && local.y <= size.height) return true;
    }
    return false;
}
