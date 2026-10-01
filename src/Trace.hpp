#pragma once

#include <Geode/Geode.hpp>

#include <fstream>
#include <string>

// Step-by-step trace written straight to <config dir>/trace.txt (flushed on every line, so
// the last line survives a crash). It is emptied at the start of every game session.
// Only the first few hits of a call site are written, so it stays small.
namespace Trace {
    inline void write(std::string const& line) {
        static bool s_started = false;
        auto path = geode::Mod::get()->getConfigDir() / "trace.txt";
        std::ofstream file(path, s_started ? std::ios::app : std::ios::trunc);
        s_started = true;
        file << line << std::endl;
        geode::log::info("ILL: {}", line);
    }
}

// Writes the line the first `limit` times this call site runs.
#define ILL_TRACE_N(limit, ...)                                         \
    do {                                                                \
        static int ill_trace_hits = 0;                                  \
        if (ill_trace_hits++ < (limit)) Trace::write(fmt::format(__VA_ARGS__)); \
    } while (0)

#define ILL_TRACE(...) Trace::write(fmt::format(__VA_ARGS__))
