#include "Bot.hpp"
#include "analysis/Analyzer.hpp"
#include "sim/SimController.hpp"
#include "sim/PlayerState.hpp"
#include "stats/Forecast.hpp"
#include "hud/FrameHud.hpp"
#include "ui/BotPopup.hpp"
#include "ui/FrameControls.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/UILayer.hpp>

#include <fmt/format.h>

using namespace geode::prelude;

namespace {
    constexpr float PROGRESS_WIDTH = 160.f;

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
    // Outside CBF mode the game's own Click Between Steps is turned off too: it applies
    // inputs between ticks, which a whole-tick replay cannot reproduce.
    void applySafeMode(GJBaseGameLayer* layer) {
        auto& bot = Bot::get();
        if (bot.mode == BotMode::Off && !bot.analyzing) return;
        layer->m_isTestMode = true;
        if (!bot.cbfMode || bot.analyzing) {
            layer->m_clickBetweenSteps = false;
            layer->m_clickOnSteps = false;
        }
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
        if (bot.mode == BotMode::Record) bot.record(this, down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isBotLayer(this)) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        auto& bot = Bot::get();
        bot.onTickStart(this);
        bot.inTick = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        bot.inTick = false;
        bot.onTickEnd(this);
    }

    void update(float dt) {
        if (!isBotLayer(this)) return GJBaseGameLayer::update(dt);
        auto layer = static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto step = [this](float stepDt) { GJBaseGameLayer::update(stepDt); };

        if (auto sim = SimController::active()) return sim->drive(layer, step);

        auto& bot = Bot::get();
        if (bot.frozen && bot.mode != BotMode::Off) {
            // Frame advance: only the requested ticks run.
            while (bot.pendingSteps > 0) {
                --bot.pendingSteps;
                for (int attempt = 0; attempt < 50; ++attempt) {
                    if (SimController::stepOnce(layer, step) > 0) break;
                }
            }
        }
        else {
            GJBaseGameLayer::update(dt);
        }
        if (auto controls = FrameControls::current()) controls->afterUpdate();
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* overlay = nullptr;
        CCLayerColor* progressBack = nullptr;
        CCLayerColor* progressFill = nullptr;
        FrameHud* hud = nullptr;
        bool warnedStartPos = false;
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

        // Progress bar of the analysis / route search, above the status label.
        auto winSize = CCDirector::get()->getWinSize();
        auto back = CCLayerColor::create({ 0, 0, 0, 150 }, PROGRESS_WIDTH + 2.f, 6.f);
        back->setPosition({ winSize.width - 7.f - PROGRESS_WIDTH, 17.f });
        back->setVisible(false);
        auto fill = CCLayerColor::create({ 255, 220, 90, 230 }, 0.f, 4.f);
        fill->setPosition({ 1.f, 1.f });
        back->addChild(fill);

        auto hud = FrameHud::create(this);
        m_fields->hud = hud;
        CCNode* parent = m_uiLayer ? static_cast<CCNode*>(m_uiLayer) : this;
        parent->addChild(hud, 9999);
        parent->addChild(FrameControls::create(this), 10001);
        parent->addChild(label, 10000);
        parent->addChild(back, 10000);
        m_fields->overlay = label;
        m_fields->progressBack = back;
        m_fields->progressFill = fill;

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
        bot.restoredPlayers = false;
        PlayLayer::resetLevel();
        bot.onReset(this);
        // Replays count ticks from the start of the level; from a start position they
        // only line up with that same start position.
        if (bot.mode != BotMode::Off && !bot.analyzing && m_startPosObject && !m_isPracticeMode && !m_fields->warnedStartPos) {
            m_fields->warnedStartPos = true;
            Notification::create("Start position active: the replay only matches runs from this same start position",
                NotificationIcon::Warning, 4.f)->show();
        }
        if (auto hud = m_fields->hud) hud->resetTo(bot.tick);
        if (auto controls = FrameControls::current()) controls->onLevelReset();
        applySafeMode(this);
        if (isHumanAttempt(this)) Forecast::onAttemptStart(this);
    }

    CheckpointObject* createCheckpoint() {
        auto checkpoint = PlayLayer::createCheckpoint();
        if (!checkpoint) return checkpoint;
        auto& bot = Bot::get();
        checkpoint->setUserObject("tick"_spr, CCInteger::create(static_cast<int>(bot.tick)));
        if (bot.mode != BotMode::Off || bot.analyzing) {
            auto players = PlayerStateHolder::create();
            if (m_player1) players->p1.save(m_player1);
            if (m_player2) players->p2.save(m_player2);
            checkpoint->setUserObject("players"_spr, players);
        }
        return checkpoint;
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        if (!checkpoint) return;
        auto& bot = Bot::get();
        if (auto saved = typeinfo_cast<CCInteger*>(checkpoint->getUserObject("tick"_spr))) {
            bot.tick = static_cast<uint32_t>(saved->getValue());
        }
        // Put back what the game's checkpoint left out, so the level continues exactly
        // like the run that made the checkpoint.
        if (bot.mode != BotMode::Off || bot.analyzing) {
            if (auto players = typeinfo_cast<PlayerStateHolder*>(checkpoint->getUserObject("players"_spr))) {
                if (m_player1) players->p1.apply(m_player1);
                if (m_player2) players->p2.apply(m_player2);
#ifdef GEODE_IS_WINDOWS
                // Held buttons are part of the snapshot only on Windows (see PlayerState.gen.hpp);
                // elsewhere they are pressed again after the reset.
                bot.restoredPlayers = true;
#endif
            }
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

        auto back = m_fields->progressBack;
        if (auto sim = SimController::active()) {
            label->setVisible(true);
            label->setString(sim->statusText().c_str());
            label->setColor({ 255, 220, 90 });
            if (back) {
                back->setVisible(true);
                float progress = std::clamp(sim->progress(), 0.f, 1.f);
                m_fields->progressFill->setContentSize({ PROGRESS_WIDTH * progress, 4.f });
            }
            return;
        }
        if (back) back->setVisible(false);
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
// the player's step is split at each input's fraction, each part is moved and
// collided, the input is applied, and the rest of the step runs. Such inputs come
// from CBF-mode recordings and from the analyzer.
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
        if (bot.splits.empty() || s_midStep || !pl) return PlayerObject::update(dt);

        // The inputs of this tick that belong to this player, in time order.
        bool dual = pl->m_gameState.m_isDualMode;
        std::vector<BotInput> mine;
        auto& pending = bot.splits;
        for (auto it = pending.begin(); it != pending.end();) {
            bool toP2 = !it->player1 && dual;
            if (this == (toP2 ? pl->m_player2 : pl->m_player1)) {
                mine.push_back(*it);
                it = pending.erase(it);
            }
            else {
                ++it;
            }
        }
        if (mine.empty()) return PlayerObject::update(dt);
        std::stable_sort(mine.begin(), mine.end(), [](auto const& a, auto const& b) { return a.subtick < b.subtick; });

        auto apply = [&](BotInput const& input) {
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
            for (auto const& input : mine) apply(input);
            return;
        }

        auto position = this->getPosition();
        float done = 0.f;
        bool first = true;
        s_midStep = true;
        for (auto const& input : mine) {
            float part = dt * input.subtick - done;
            if (part > 0.f) {
                PlayerObject::update(part);
                if (first && ((m_yVelocity < 0) ^ m_isUpsideDown)) m_isOnGround = startedOnGround;
                if (!m_isOnSlope || m_isDart) pl->checkCollisions(this, 0.f, true);
                else pl->checkCollisions(this, dt, true);
                PlayerObject::updateRotation(part);
                resetCollisionLog(this);
                done += part;
                first = false;
            }
            apply(input);
        }
        float rest = std::max(dt - done, 0.f);
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

class $modify(BotUILayer, UILayer) {
    // Taps on the frame controls are button presses, not jumps.
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        if (auto controls = FrameControls::current(); controls && controls->hitsTouch(touch)) return false;
        return UILayer::ccTouchBegan(touch, event);
    }

    // Keyboard frame controls while the bot is on: F freeze, V one tick, C rewind.
    void keyDown(enumKeyCodes key, double timestamp) {
        auto controls = FrameControls::current();
        auto& bot = Bot::get();
        if (controls && bot.frameControls && bot.mode != BotMode::Off && !SimController::active()) {
            switch (key) {
                case KEY_F: return controls->toggleFreeze();
                case KEY_V: return controls->step(1);
                case KEY_C: return controls->rewind();
                default: break;
            }
        }
        UILayer::keyDown(key, timestamp);
    }
};

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = Bot::get();
        if (bot.speed != 1.f && PlayLayer::get() && !SimController::active()) dt *= bot.speed;
        CCScheduler::update(dt);
    }
};
