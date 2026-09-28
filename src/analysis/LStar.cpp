#include "LStar.hpp"
#include "../Bot.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr double TARGET_TIME = 86400.0;  // 24 hours
    constexpr double MIN_L = 0.001;
    constexpr double MAX_L = 1e15;
    constexpr double WINDOW_SCALE = 0.5 * 0.7071067811865475;  // 1 / (2 * sqrt(2))

    std::atomic<int> s_generation = 0;
    std::atomic<bool> s_computing = false;

    struct Sample {
        double time;    // seconds since the start of the attempt
        double weight;  // window in seconds * WINDOW_SCALE
    };

    // Expected time to beat inputs [0, last] at precision L.
    double expectedTime(std::vector<Sample> const& s, size_t first, size_t last, double L) {
        if (!(L > 0.0)) return 1e100;
        double reach = 1.0;      // probability of reaching input j
        double failTime = 0.0;   // sum of time lost on attempts that fail at j
        for (size_t j = first; j <= last; ++j) {
            double x = s[j].weight * L;
            if (x > 6.0) continue;  // erfc(6) ~ 2e-17: never missed
            double miss = std::erfc(x);
            double hit = std::max(1.0 - miss, 1e-15);
            failTime += s[j].time * reach * miss;
            reach *= hit;
            if (reach < 1e-200) return 1e100;
        }
        return (s[last].time * reach + failTime) / reach;
    }

    // Smallest L whose expected time is at most the target, searching upward from `hint`.
    double solve(std::vector<Sample> const& s, size_t& first, size_t last, double hint) {
        double low = std::max(hint, MIN_L);
        // Inputs that are certain at this precision stay certain for any larger L.
        while (first < last && s[first].weight * low > 6.5) ++first;

        double high = low * 2.0 + 1.0;
        while (expectedTime(s, first, last, high) > TARGET_TIME && high < MAX_L) high *= 2.0;
        if (expectedTime(s, first, last, low) <= TARGET_TIME) return low;

        for (int iter = 0; iter < 80 && (high - low) > 1e-6 * high; ++iter) {
            double mid = (low + high) * 0.5;
            if (expectedTime(s, first, last, mid) > TARGET_TIME) low = mid;
            else high = mid;
        }
        return std::clamp((low + high) * 0.5, MIN_L, MAX_L);
    }
}

namespace LStar {
    bool isComputing() {
        return s_computing;
    }

    void computeAsync() {
        // Any calculation still running is now stale.
        ++s_generation;
        s_computing = false;

        auto& replay = Bot::get().replay;
        replay.lstar.clear();
        replay.lstarShare.clear();
        if (!replay.hasAnalysis()) return;

        double fps = replay.effectiveTps();
        std::vector<Sample> samples;
        std::vector<size_t> sampleOf(replay.inputs.size(), SIZE_MAX);
        for (size_t i = 0; i < replay.inputs.size(); ++i) {
            auto const& a = replay.analysis[i];
            if (!a.analyzed || a.unreliable) continue;
            sampleOf[i] = samples.size();
            samples.push_back({ replay.inputs[i].tick / fps, a.window() / fps * WINDOW_SCALE });
        }
        if (samples.empty()) return;

        int generation = ++s_generation;
        s_computing = true;
        size_t inputCount = replay.inputs.size();

        std::thread([generation, samples = std::move(samples), sampleOf = std::move(sampleOf), inputCount] {
            std::vector<double> perSample(samples.size());
            size_t first = 0;
            double hint = MIN_L;
            for (size_t k = 0; k < samples.size(); ++k) {
                if (s_generation != generation) return;
                hint = solve(samples, first, k, hint);
                perSample[k] = hint;
            }

            // Share of the whole level's difficulty reached after each input: the
            // accumulated -log(hit probability) at the final L*.
            double finalL = perSample.back();
            std::vector<double> cumulative(samples.size());
            double total = 0.0;
            for (size_t k = 0; k < samples.size(); ++k) {
                double hit = std::max(1.0 - std::erfc(samples[k].weight * finalL), 1e-300);
                total += -std::log(hit);
                cumulative[k] = total;
            }

            std::vector<double> lstar(inputCount, 0.0), share(inputCount, 0.0);
            double lastL = 0.0, lastShare = 0.0;
            for (size_t i = 0; i < inputCount; ++i) {
                if (sampleOf[i] != SIZE_MAX) {
                    lastL = perSample[sampleOf[i]];
                    lastShare = total > 0.0 ? cumulative[sampleOf[i]] / total * 100.0 : 0.0;
                }
                lstar[i] = lastL;
                share[i] = lastShare;
            }

            queueInMainThread([generation, lstar = std::move(lstar), share = std::move(share)]() mutable {
                if (s_generation != generation) return;
                auto& replay = Bot::get().replay;
                if (replay.inputs.size() == lstar.size()) {
                    replay.lstar = std::move(lstar);
                    replay.lstarShare = std::move(share);
                }
                s_computing = false;
            });
        }).detach();
    }
}
