#include "Forecast.hpp"
#include "../Bot.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <tuple>

using namespace geode::prelude;

namespace {
    constexpr double WINDOW_SCALE = 0.5 * 0.7071067811865475;  // same as LStar
    constexpr double RESPAWN_SECONDS = 1.0;
    constexpr size_t MIN_ATTEMPTS = 5;
    constexpr size_t MIN_DEATHS = 3;

    struct Attempt {
        float startX = 0.f;
        float endX = 0.f;
        int outcome = 2;
    };

    int s_levelID = -1;
    std::vector<Attempt> s_attempts;
    bool s_open = false;
    Attempt s_current;

    bool s_dirty = true;
    std::tuple<int, size_t, size_t, bool> s_signature;
    Forecast::Estimate s_estimate;

    std::filesystem::path statsFile(int levelID) {
        return Mod::get()->getSaveDir() / "stats" / fmt::format("{}.txt", levelID);
    }

    int levelOf(PlayLayer* layer) {
        return layer && layer->m_level ? layer->m_level->m_levelID.value() : 0;
    }

    void loadLevel(int levelID) {
        if (levelID == s_levelID) return;
        s_levelID = levelID;
        s_attempts.clear();
        s_open = false;
        s_dirty = true;
        auto data = file::readString(statsFile(levelID));
        if (!data) return;
        std::istringstream in(data.unwrap());
        Attempt a;
        while (in >> a.startX >> a.endX >> a.outcome) s_attempts.push_back(a);
    }

    void append(int levelID, Attempt const& a) {
        auto path = statsFile(levelID);
        (void)file::createDirectoryAll(path.parent_path());
        std::string existing = file::readString(path).unwrapOr("");
        existing += fmt::format("{} {} {}\n", a.startX, a.endX, a.outcome);
        (void)file::writeStringSafe(path, existing);
    }

    float playerX(PlayLayer* layer) {
        return layer && layer->m_player1 ? layer->m_player1->getPositionX() : 0.f;
    }

    // Window (seconds * scale) of each input, 0 when unusable.
    std::vector<double> weights() {
        auto const& bot = Bot::get();
        auto const& replay = bot.replay;
        std::vector<double> w(replay.inputs.size(), 0.0);
        if (!replay.hasAnalysis()) return w;
        double fps = replay.effectiveTps();
        for (size_t i = 0; i < w.size(); ++i) {
            auto const& a = replay.analysis[i];
            if (!a.analyzed || a.unreliable || a.x <= 0.f) continue;
            w[i] = replay.windowOf(i, bot.useCbf) / fps * WINDOW_SCALE;
        }
        return w;
    }

    void recompute() {
        s_estimate = {};
        s_dirty = false;
        auto const& replay = Bot::get().replay;
        if (!replay.hasAnalysis() || replay.levelID != s_levelID) return;

        auto w = weights();
        size_t n = w.size();
        // How many attempts hit / missed each input (difference arrays over input ranges).
        std::vector<double> passDiff(n + 1, 0.0), fail(n, 0.0);
        size_t deaths = 0;
        for (auto const& a : s_attempts) {
            size_t first = 0;
            while (first < n && replay.analysis[first].x < a.startX - 1.f) ++first;
            size_t last = first;  // inputs [first, last) were hit
            while (last < n && replay.analysis[last].x <= a.endX) ++last;
            if (a.outcome == 0) {
                // The last measured input before the death is the one that was missed.
                size_t missed = last;
                while (missed > first && w[missed - 1] <= 0.0) --missed;
                if (missed > first) {
                    fail[missed - 1] += 1.0;
                    last = missed - 1;
                    ++deaths;
                }
            }
            else if (a.outcome == 1) {
                last = n;
            }
            if (last > first) {
                passDiff[first] += 1.0;
                passDiff[last] -= 1.0;
            }
        }
        std::vector<double> pass(n, 0.0);
        double running = 0.0;
        for (size_t i = 0; i < n; ++i) {
            running += passDiff[i];
            pass[i] = running;
        }

        s_estimate.attempts = s_attempts.size();
        s_estimate.deaths = deaths;
        if (s_attempts.size() < MIN_ATTEMPTS || deaths < MIN_DEATHS) return;

        auto logLikelihood = [&](double L) {
            double ll = 0.0;
            for (size_t i = 0; i < n; ++i) {
                if (w[i] <= 0.0 || (pass[i] == 0.0 && fail[i] == 0.0)) continue;
                double miss = std::erfc(w[i] * L);
                double hit = 1.0 - miss;
                if (pass[i] > 0.0) ll += pass[i] * std::log(std::max(hit, 1e-300));
                if (fail[i] > 0.0) ll += fail[i] * std::log(std::max(miss, 1e-300));
            }
            return ll;
        };

        // Golden-section search for the most likely L on a log scale.
        double lo = std::log(1.0), hi = std::log(1e5);
        const double ratio = 0.6180339887498949;
        double a = hi - ratio * (hi - lo), b = lo + ratio * (hi - lo);
        double fa = logLikelihood(std::exp(a)), fb = logLikelihood(std::exp(b));
        for (int iter = 0; iter < 80; ++iter) {
            if (fa < fb) { lo = a; a = b; fa = fb; b = lo + ratio * (hi - lo); fb = logLikelihood(std::exp(b)); }
            else { hi = b; b = a; fb = fa; a = hi - ratio * (hi - lo); fa = logLikelihood(std::exp(a)); }
        }
        double L = std::exp((lo + hi) * 0.5);

        // Expected time to beat the level from the start at this precision (as for L*).
        double fps = replay.effectiveTps();
        double reach = 1.0, failTime = 0.0, endTime = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double t = replay.inputs[i].tick / fps;
            endTime = std::max(endTime, t);
            if (w[i] <= 0.0) continue;
            double miss = std::erfc(w[i] * L);
            failTime += t * reach * miss;
            reach *= std::max(1.0 - miss, 1e-300);
        }
        double expectedAttempts = 1.0 / std::max(reach, 1e-300);
        double seconds = (endTime * reach + failTime) / std::max(reach, 1e-300);

        s_estimate.valid = true;
        s_estimate.precision = L;
        s_estimate.expectedAttempts = expectedAttempts;
        s_estimate.expectedSeconds = seconds + (expectedAttempts - 1.0) * RESPAWN_SECONDS;
    }
}

namespace Forecast {
    void onAttemptStart(PlayLayer* layer) {
        if (s_open) onAttemptEnd(layer, 2);
        loadLevel(levelOf(layer));
        s_current = { playerX(layer), playerX(layer), 2 };
        s_open = true;
    }

    void onAttemptEnd(PlayLayer* layer, int outcome) {
        if (!s_open) return;
        s_open = false;
        s_current.endX = playerX(layer);
        s_current.outcome = outcome;
        // Restarting right away tells nothing about any input.
        if (outcome == 2 && s_current.endX - s_current.startX < 30.f) return;
        loadLevel(levelOf(layer));
        s_attempts.push_back(s_current);
        append(s_levelID, s_current);
        s_dirty = true;
    }

    Estimate const& estimate() {
        auto const& bot = Bot::get();
        auto const& replay = bot.replay;
        if (replay.levelID && replay.levelID != s_levelID) loadLevel(replay.levelID);
        auto signature = std::make_tuple(replay.levelID, replay.inputs.size(), replay.analysis.size(), bot.useCbf);
        if (s_dirty || signature != s_signature) {
            s_signature = signature;
            recompute();
        }
        return s_estimate;
    }

    std::vector<double> missChances(double precision) {
        auto w = weights();
        std::vector<double> miss(w.size(), 0.0);
        for (size_t i = 0; i < w.size(); ++i) {
            if (w[i] > 0.0) miss[i] = std::erfc(w[i] * precision);
        }
        return miss;
    }
}
