#pragma once

// L* ("NaNDL precision") of a replay, from the frame windows found by the analyzer.
//
// Model (from NaN's Frame Window Counter, MIT licensed,
// https://github.com/hyper-5/frame-window-counter): the player's timing error is
// normally distributed with standard deviation 1/L, so an input with a window of
// w seconds is hit with probability erf(w * L / (2 * sqrt(2))). L* is the precision
// at which the expected time to beat the level, retrying from the start after
// every death, equals the target time (24 hours by default).
namespace LStar {
    // Recomputes Bot::get().replay.lstar / lstarShare on a worker thread.
    void computeAsync();
    bool isComputing();
}
