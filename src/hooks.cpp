#include "Bot.hpp"
#include "analysis/Analyzer.hpp"
#include "sim/SimController.hpp"
#include "stats/Forecast.hpp"
#include "hud/FrameHud.hpp"
#include "ui/BotPopup.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <fmt/format.h>

using namespace geode::prelude;

namespace {
    bool isBotLayer(GJBaseGameLayer* layer) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
    }

    // Attempts that count for the personal forecast: the player's own, in normal mode
    // (from the start or a start position). Practice is only used to build macros.
    bool isHumanAttempt(PlayLayer* layer) {
        return Bot::get().mode == BotMode::Off && !SimController::active() && !layer->m_isPracticeMode;
    }

    // With the bot in use nothing gets saved: no stars, no new best, no completion.
    // The game's own Click Between Steps is turned off too: it applies inputs between
    // ticks, which a tick-based replay cannot reproduce.
    void applySafeMode(GJBaseGameLayer* layer) {
        if (Bot::get().mode == BotMode::Off && !Bot::get().analyzing) return;
        layer->m_isTestMode = true;
        layer->m_clickBetweenSteps = false;
        layer->m_clickOnSteps = false;
    }
}

class $modify(BotGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& bot = Bot::get();
        if (!isBotLayer(this) || bot.injecting) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        // During playback and analysis the player's own clicks are ignored.
        if (bot.mode == BotMode::Play || bot.analyzing) return;
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

    void update(float dt) {
        auto sim = SimController::active();
        if (sim && isBotLayer(this)) {
            sim->drive(static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this)), [this](float step) {
                GJBaseGameLayer::update(step);
            });
            return;
        }
        GJBaseGameLayer::update(dt);
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* overlay = nullptr;
        FrameHud* hud = nullptr;
    };

    static FrameHud* currentHud() {
        auto pl = PlayLayer::get();
        return pl ? static_cast<BotPlayLayer*>(pl)->m_fields->hud : nullptr;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        Bot::get().onLevelStart(level);
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({ 1.f, 0.f });
        label->setScale(0.3f);
        label->setOpacity(210);
        label->setPosition({ CCDirector::get()->getWinSize().width - 6.f, 4.f });
        label->setID("overlay"_spr);

        auto hud = FrameHud::create(this);
        m_fields->hud = hud;
        if (m_uiLayer) {
            m_uiLayer->addChild(hud, 9999);
            m_uiLayer->addChild(label, 10000);
        }
        else {
            this->addChild(hud, 9999);
            this->addChild(label, 10000);
        }
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
        if (auto hud = m_fields->hud) hud->resetTo(bot.tick);
        applySafeMode(this);
        if (isHumanAttempt(this)) Forecast::onAttemptStart(this);
    }

    CheckpointObject* createCheckpoint() {
        auto checkpoint = PlayLayer::createCheckpoint();
        if (checkpoint) {
            checkpoint->setUserObject("tick"_spr, CCInteger::create(static_cast<int>(Bot::get().tick)));
        }
        return checkpoint;
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        if (!checkpoint) return;
        if (auto saved = typeinfo_cast<CCInteger*>(checkpoint->getUserObject("tick"_spr))) {
            Bot::get().tick = static_cast<uint32_t>(saved->getValue());
        }
    }

    void levelComplete() {
        // During a simulation the end is detected on the tick it happens (Bot::onTickEnd);
        // this call comes after the end animation and the level must not actually finish.
        if (SimController::active()) return;
        if (isHumanAttempt(this)) Forecast::onAttemptEnd(this, 1);
        applySafeMode(this);
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The anti-cheat spike always goes through to the game.
        if (object != m_anticheatSpike) {
            if (auto sim = SimController::active()) return sim->onDeath();
            if (isHumanAttempt(this)) Forecast::onAttemptEnd(this, 0);
        }
        applySafeMode(this);
        PlayLayer::destroyPlayer(player, object);
    }

    void onQuit() {
        if (auto sim = SimController::active()) sim->cancel(nullptr);
        if (isHumanAttempt(this)) Forecast::onAttemptEnd(this, 2);
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

        if (auto sim = SimController::active()) {
            label->setVisible(true);
            label->setString(sim->statusText().c_str());
            label->setColor({ 255, 220, 90 });
            return;
        }
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

$execute {
    Bot::get().onInputPlayed = [](size_t index) {
        if (auto hud = BotPlayLayer::currentHud()) hud->onInputPlayed(index);
    };
}

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

// Inputs between ticks, the way Click Between Frames does it (MIT, theyareonit/Click-Between-Frames):
// the player's step is split at the input's fraction, the first part is moved and
// collided, the input is applied, and the rest of the step runs. Only the analyzer
// places inputs between ticks.
namespace {
    bool s_midStep = false;
    struct RotationFix {
        PlayerObject* player = nullptr;
        float delta = 0.f;
        CCPoint position;
    } s_rotationFix;

    void resetCollisionLog(PlayerObject* p) {
        p->m_collisionLogTop->removeAllObjects();
        p->m_collisionLogBottom->removeAllObjects();
        p->m_collisionLogLeft->removeAllObjects();
        p->m_collisionLogRight->removeAllObjects();
        p->m_lastCollisionLeft = -1;
        p->m_lastCollisionRight = -1;
        p->m_lastCollisionBottom = -1;
        p->m_lastCollisionTop = -1;
    }
}

class $modify(BotPlayerObject, PlayerObject) {
    void update(float dt) {
        auto& bot = Bot::get();
        auto pl = PlayLayer::get();
        if (!bot.split.active || s_midStep || !pl) return PlayerObject::update(dt);

        auto const& input = bot.split.input;
        bool toP2 = !input.player1 && pl->m_gameState.m_isDualMode;
        if (this != (toP2 ? pl->m_player2 : pl->m_player1)) return PlayerObject::update(dt);

        bot.split.active = false;
        auto apply = [&] {
            bot.injecting = true;
            pl->handleButton(input.down, input.button, input.player1);
            bot.injecting = false;
        };

        // While a click is only being buffered (in the air, nothing to hit) splitting
        // changes nothing, and CBF itself falls back to the step boundary.
        bool startedOnGround = m_isOnGround;
        bool notBuffering = startedOnGround || m_touchingRings->count() || m_isDashing
            || m_isDart || m_isBird || m_isShip || m_isSwing;
        if (!notBuffering) {
            PlayerObject::update(dt);
            return apply();
        }

        auto position = this->getPosition();
        float first = dt * input.subtick;
        s_midStep = true;
        PlayerObject::update(first);
        if ((m_yVelocity < 0) ^ m_isUpsideDown) m_isOnGround = startedOnGround;
        if (!m_isOnSlope || m_isDart) pl->checkCollisions(this, 0.f, true);
        else pl->checkCollisions(this, dt, true);
        PlayerObject::updateRotation(first);
        resetCollisionLog(this);
        apply();
        float rest = dt - first;
        PlayerObject::update(rest);
        s_midStep = false;
        s_rotationFix = { this, rest, position };
    }

    void updateRotation(float dt) {
        if (s_rotationFix.player == this && !s_midStep) {
            // Finish the rotation left incomplete by the split step.
            auto fix = s_rotationFix;
            s_rotationFix.player = nullptr;
            PlayerObject::updateRotation(fix.delta);
            m_lastPosition = fix.position;
            return;
        }
        PlayerObject::updateRotation(dt);
    }
};

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = Bot::get();
        if (bot.speed != 1.f && PlayLayer::get() && !SimController::active()) dt *= bot.speed;
        CCScheduler::update(dt);
    }
};
