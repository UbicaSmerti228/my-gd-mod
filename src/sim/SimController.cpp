#include "SimController.hpp"
#include "../analysis/Analyzer.hpp"
#include "RouteFinder.hpp"

#include <map>

using namespace geode::prelude;

SimController* SimController::active() {
    if (Analyzer::get().isActive()) return &Analyzer::get();
    if (RouteFinder::get().isActive()) return &RouteFinder::get();
    return nullptr;
}

void SimController::repressHeld(GJBaseGameLayer* layer) {
    auto& bot = Bot::get();
    std::map<std::pair<uint8_t, bool>, bool> held;
    for (auto const& i : bot.simInputs) {
        if (i.tick >= bot.tick) break;
        held[{ i.button, i.player1 }] = i.down;
    }
    for (auto const& [key, isDown] : held) {
        if (!isDown) continue;
        bot.injecting = true;
        layer->handleButton(true, key.first, key.second);
        bot.injecting = false;
    }
}

void SimController::restore(PlayLayer* layer, CheckpointObject* checkpoint) {
    static size_t s_restores = 0;
    if (++s_restores <= 5) geode::log::info("ILL: restore #{} (checkpoint {})", s_restores, static_cast<void*>(checkpoint));
    Bot::get().injecting = false;
    Bot::get().splits.clear();
    layer->m_checkpointArray->removeAllObjects();
    if (checkpoint) layer->m_checkpointArray->addObject(checkpoint);
    layer->resetLevel();
}

uint32_t SimController::stepOnce(PlayLayer* layer, std::function<void(float)> const& step) {
    auto& bot = Bot::get();
    uint32_t before = bot.tick;
    // One update must run exactly one physics tick, so the carried-over time is dropped.
    layer->m_extraDelta = 0.0;
    float warp = std::min(layer->m_gameState.m_timeWarp, 1.f);
    if (!(warp > 0.f)) warp = 1.f;
    step(warp / static_cast<float>(bot.replay.effectiveTps()));
    return bot.tick >= before ? bot.tick - before : 0;
}

void SimController::preparePractice(PlayLayer* layer) {
    // The simulations restore the level from checkpoints, which the game only does in
    // practice mode. The game's own practice switch (togglePracticeMode) restarts the
    // level and rebuilds the practice interface, and it crashed in both places it was
    // tried, so only the flag is set: nothing is shown, and the level itself is
    // restarted by the simulation as it does for every restore.
    m_wasPractice = layer->m_isPracticeMode;
    layer->m_isPracticeMode = true;
    geode::log::info("ILL: simulation prepared (practice flag set, was {})", m_wasPractice);
}

void SimController::enterSimulation(PlayLayer*) {
    if (m_entered) return;
    m_entered = true;
    if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
        engine->m_globalChannel->getVolume(&m_oldVolume);
        engine->m_globalChannel->setVolume(0.f);
    }
}

void SimController::leaveSimulation(PlayLayer* layer) {
    auto& bot = Bot::get();
    bot.analyzing = false;
    bot.simInputs.clear();
    bot.splits.clear();
    if (!m_entered) return;
    m_entered = false;
    if (layer) {
        layer->m_checkpointArray->removeAllObjects();
        layer->m_isPracticeMode = m_wasPractice;
        layer->resetLevel();
        geode::log::info("ILL: simulation left");
    }
    if (auto engine = FMODAudioEngine::get(); engine && engine->m_globalChannel) {
        engine->m_globalChannel->setVolume(m_oldVolume);
    }
}
