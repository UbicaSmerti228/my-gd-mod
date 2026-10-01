#pragma once

#include <Geode/Geode.hpp>

#include <cstddef>
#include <vector>

// Personal forecast from the player's own attempts.
//
// Every normal-mode attempt (from the start or from a start position; practice is
// ignored) is logged per level as where it started, where it ended, and how. With the
// frame windows of an analyzed replay of the level, each attempt says which inputs the
// player hit and which one they missed. Under NaN's timing model (see LStar.hpp) the
// player's precision L is the value that makes those hits and misses most likely.
// From L follows the chance of every input, the expected number of full attempts and
// the expected play time to beat the level.
namespace Forecast {
    struct Estimate {
        bool valid = false;
        double precision = 0.0;       // the player's L
        size_t attempts = 0;
        size_t deaths = 0;
        double expectedAttempts = 0.0;  // full-level attempts from the start
        double expectedSeconds = 0.0;   // play time including a respawn per death
    };

    void onAttemptStart(PlayLayer* layer);
    // outcome: 0 = died, 1 = beat the level, 2 = left or restarted without dying
    void onAttemptEnd(PlayLayer* layer, int outcome);

    Estimate const& estimate();
    // Chance of missing each input of the loaded replay at precision L (0 where unknown).
    std::vector<double> missChances(double precision);
}
