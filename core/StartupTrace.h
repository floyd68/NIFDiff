// StartupTrace.h - lightweight startup-phase timing on top of NifLog.
//
// Every line carries two numbers: the phase's own duration and "t+", the
// wall-clock offset from the first StartupTrace use in the process (i.e.
// the top of RunNIFDiffApp). Grep the log for [STARTUP] to get a complete,
// ordered picture of where launch time goes.
#pragma once

#include "NifLog.h"

#include <chrono>
#include <string>
#include <utility>

namespace nsk::StartupTrace
{

using Clock = std::chrono::steady_clock;

// Anchored on first use - RunNIFDiffApp calls SinceStartMs() at its top so
// every later "t+" offset is relative to app-entry, not to some first log
// call deep inside a phase.
inline Clock::time_point ProcessStart()
{
    static const Clock::time_point s_start = Clock::now();
    return s_start;
}

inline double SinceStartMs()
{
    return std::chrono::duration<double, std::milli>(Clock::now() - ProcessStart()).count();
}

// One-shot timestamp ("we are here at t+X ms").
inline void Mark(const char* label)
{
    NIFLOG_INFO("[STARTUP] {:<44} -            (t+{:>7.1f} ms)", label, SinceStartMs());
}

// RAII phase timer: logs the enclosed scope's duration on destruction.
class Phase
{
public:
    explicit Phase(std::string label)
        : m_label(std::move(label))
        , m_begin(Clock::now())
    {
    }

    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;

    ~Phase()
    {
        const double ms = ElapsedMs();
        NIFLOG_INFO("[STARTUP] {:<44} {:>9.2f} ms (t+{:>7.1f} ms)", m_label, ms, SinceStartMs());
    }

    double ElapsedMs() const
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - m_begin).count();
    }

private:
    std::string m_label;
    Clock::time_point m_begin;
};

} // namespace nsk::StartupTrace
