#pragma once

#include "../Bot.hpp"

// GDR2 (.gdr2), the replay format shared by Eclipse Menu, xdBot and other bots
// (https://github.com/maxnut/GDReplayFormat). A GDR frame is a tick counted from 0,
// which is the same tick this bot counts. Inputs between ticks (CBF mode) have no
// place in plain GDR2 and are rounded to their tick on export.
namespace Gdr {
    constexpr char const* EXTENSION = ".gdr2";

    geode::Result<Replay> read(std::filesystem::path const& path);
    // Returns how many inputs lost their position between ticks.
    geode::Result<size_t> write(Replay const& replay, std::filesystem::path const& path);
}
