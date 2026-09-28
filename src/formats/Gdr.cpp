#include "Gdr.hpp"

#include <gdr/gdr.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <span>

using namespace geode::prelude;

namespace {
    using GdrReplay = gdr::Replay<>;

    // Other bots may count frames one tick apart; adjustable without a rebuild.
    int64_t frameOffset() {
        return Mod::get()->getSavedValue<int64_t>("gdr-frame-offset", 0);
    }
}

namespace Gdr {
    Result<Replay> read(std::filesystem::path const& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return Err("Could not open the file");
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        auto parsed = GdrReplay::importData(std::span<uint8_t>(data));
        if (parsed.isErr()) return Err(fmt::format("Not a GDR2 replay: {}", parsed.unwrapErr()));
        auto const& gdr = parsed.unwrap();
        if (gdr.platformer) return Err("Platformer replays are not supported");

        Replay replay;
        replay.levelID = static_cast<int>(gdr.levelInfo.id);
        replay.levelName = gdr.levelInfo.name;
        replay.tps = static_cast<int>(std::lround(gdr.framerate > 0.0 ? gdr.framerate : 240.0));
        int64_t offset = frameOffset();
        for (auto const& input : gdr.inputs) {
            int64_t tick = static_cast<int64_t>(input.frame) + offset;
            if (tick < 0) continue;
            replay.inputs.push_back({
                static_cast<uint32_t>(tick), input.button, !input.player2, input.down, 0.f
            });
        }
        std::stable_sort(replay.inputs.begin(), replay.inputs.end(), [](auto const& a, auto const& b) {
            return a.tick < b.tick;
        });
        return Ok(std::move(replay));
    }

    Result<size_t> write(Replay const& replay, std::filesystem::path const& path) {
        GdrReplay gdr("ILL Replay Bot", 1);
        gdr.gameVersion = GEODE_COMP_GD_VERSION;
        gdr.framerate = replay.effectiveTps();
        gdr.levelInfo = gdr::Level(replay.levelName, static_cast<uint32_t>(std::max(replay.levelID, 0)));
        gdr.duration = static_cast<float>(replay.lastTick() / static_cast<double>(replay.effectiveTps()));

        size_t rounded = 0;
        int64_t offset = frameOffset();
        for (auto const& input : replay.inputs) {
            if (input.subtick > 0.f) ++rounded;
            int64_t frame = static_cast<int64_t>(input.tick) - offset;
            if (frame < 0) continue;
            gdr.inputs.emplace_back(static_cast<uint64_t>(frame), input.button, !input.player1, input.down);
        }

        auto data = gdr.exportData();
        if (data.isErr()) return Err(data.unwrapErr());
        std::ofstream file(path, std::ios::binary);
        if (!file) return Err("Could not write the file");
        auto const& bytes = data.unwrap();
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return Err("Could not write the file");
        return Ok(rounded);
    }
}
