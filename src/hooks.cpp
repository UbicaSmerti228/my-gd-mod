#include "Bot.hpp"
#include "ui/BotPopup.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <fmt/format.h>

using namespace geode::prelude;

namespace {
    bool isBotLayer(GJBaseGameLayer* layer) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
    }

    // With the bot in use nothing gets saved: no stars, no new best, no completion.
    void applySafeMode(GJBaseGameLayer* layer) {
        if (Bot::get().mode != BotMode::Off) layer->m_isTestMode = true;
    }
}

class $modify(BotGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& bot = Bot::get();
        if (!isBotLayer(this) || bot.injecting) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        // During playback the player's own clicks are ignored.
        if (bot.mode == BotMode::Play) return;
        if (bot.mode == BotMode::Record) bot.record(down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isBotLayer(this)) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        auto& bot = Bot::get();
        bot.onTickStart(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        bot.onTickEnd(this);
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* overlay = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        Bot::get().onLevelStart(level);
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto winSize = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 0.f });
        label->setScale(0.35f);
        label->setOpacity(210);
        label->setPosition({ 6.f, 4.f });
        label->setZOrder(1000);
        label->setID("overlay"_spr);
        this->addChild(label);
        m_fields->overlay = label;

        this->schedule(schedule_selector(BotPlayLayer::updateOverlay), 0.1f);
        this->updateOverlay(0.f);
        Bot::get().setSpeed(Bot::get().speed);
        applySafeMode(this);
        return true;
    }

    void resetLevel() {
        auto& bot = Bot::get();
        // loadFromCheckpoint (called from inside resetLevel in practice) sets the real value.
        bot.tick = 0;
        PlayLayer::resetLevel();
        bot.onReset(this);
        applySafeMode(this);
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        if (checkpoint) {
            checkpoint->setUserObject("tick"_spr, CCInteger::create(static_cast<int>(Bot::get().tick)));
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        if (!checkpoint) return;
        if (auto saved = typeinfo_cast<CCInteger*>(checkpoint->getUserObject("tick"_spr))) {
            Bot::get().tick = static_cast<uint32_t>(saved->getValue());
        }
    }

    void levelComplete() {
        applySafeMode(this);
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        applySafeMode(this);
        PlayLayer::destroyPlayer(player, object);
    }

    void onQuit() {
        // Leave the rest of the game at normal speed and pitch.
        if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
            engine->m_globalChannel->setPitch(1.f);
        }
        PlayLayer::onQuit();
    }

    void updateOverlay(float) {
        auto label = m_fields->overlay;
        if (!label) return;
        auto& bot = Bot::get();
        if (!bot.showOverlay) {
            label->setVisible(false);
            return;
        }
        label->setVisible(true);

        std::string tps = bot.measuredTps > 0 ? fmt::format("{} TPS", bot.measuredTps) : "-- TPS";
        std::string speed = bot.speed != 1.f ? fmt::format("  x{:.2f}", bot.speed) : "";
        switch (bot.mode) {
            case BotMode::Record:
                label->setString(fmt::format("BOT REC  {}  tick {}  inputs {}{}",
                    tps, bot.tick, bot.replay.inputs.size(), speed).c_str());
                label->setColor({ 255, 90, 90 });
                break;
            case BotMode::Play:
                label->setString(fmt::format("BOT PLAY  {}  tick {} / {}{}",
                    tps, bot.tick, bot.replay.lastTick(), speed).c_str());
                label->setColor({ 110, 255, 140 });
                break;
            case BotMode::Off:
                label->setString(fmt::format("{}{}", tps, speed).c_str());
                label->setColor({ 255, 255, 255 });
                break;
        }
    }
};

class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto sprite = CircleButtonSprite::create(
            CCLabelBMFont::create("BOT", "bigFont.fnt"),
            CircleBaseColor::DarkPurple, CircleBaseSize::Medium
        );
        sprite->setScale(0.8f);
        auto button = CCMenuItemExt::createSpriteExtra(sprite, [](CCMenuItemSpriteExtra*) {
            BotPopup::create()->show();
        });
        button->setID("bot-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        }
        else {
            auto winSize = CCDirector::get()->getWinSize();
            auto fallback = CCMenu::create();
            fallback->setPosition({ winSize.width - 30.f, winSize.height / 2.f });
            fallback->addChild(button);
            this->addChild(fallback);
        }
    }
};

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = Bot::get();
        if (bot.speed != 1.f && PlayLayer::get()) dt *= bot.speed;
        CCScheduler::update(dt);
    }
};
