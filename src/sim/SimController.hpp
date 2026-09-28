#pragma once

#include "../Bot.hpp"

#include <Geode/Geode.hpp>

#include <functional>
#include <string>

// Something that takes over the level to simulate it tick by tick: the frame window
// analyzer and the route finder. While one is active the game update, deaths and
// level completion are routed to it (see hooks.cpp), the level runs in practice mode
// with the sound muted, and the player's own clicks are ignored.
class SimController {
public:
    virtual ~SimController() = default;

    virtual bool isActive() const = 0;
    virtual std::string statusText() const = 0;
    virtual void cancel(PlayLayer* layer) = 0;

    // Called instead of the normal game update; `step` runs one original update.
    virtual void drive(PlayLayer* layer, std::function<void(float)> const& step) = 0;
    virtual void onTickEnd(GJBaseGameLayer* layer, uint32_t tick) = 0;
    virtual void onReset(GJBaseGameLayer* layer) { repressHeld(layer); }
    virtual void onDeath() = 0;
    virtual void onComplete() = 0;

    // The controller currently running, if any.
    static SimController* active();

protected:
    // Buttons held across a checkpoint are pressed again, like holding through a respawn.
    static void repressHeld(GJBaseGameLayer* layer);
    // Restores the level to `checkpoint`, or to the start when it is null.
    static void restore(PlayLayer* layer, CheckpointObject* checkpoint);
    // Runs one update that advances physics by at most one tick; returns the ticks advanced.
    static uint32_t stepOnce(PlayLayer* layer, std::function<void(float)> const& step);

    void enterSimulation(PlayLayer* layer);
    void leaveSimulation(PlayLayer* layer);

    bool m_wasPractice = false;
    float m_oldVolume = 1.f;
    bool m_entered = false;
};
