// LateBoundSignal.h - a thread-safe notification whose receiver is attached
// after the sender may already have fired.
//
// The startup archive scan has to be wired to its completion callback before
// it starts (its worker must never read a std::function the UI thread is
// still assigning), but the receiver - the backplate's async-redraw token -
// only exists once the window has been created, and the scan deliberately
// overlaps that. The scan therefore gets `[s] { s->Fire(); }` up front and
// the UI binds the real target later:
//
//   Fire() before Bind(): remembered; Bind() invokes the target once.
//   Fire() after Bind():  invokes the target.
//   Reset():              drops the target and any pending fire.
//
// The target runs outside the lock, on whichever thread called Fire() (or
// Bind() for a remembered fire), so it must be safe to call from any thread.
#pragma once

#include <functional>
#include <mutex>
#include <utility>

namespace nsk
{

class LateBoundSignal
{
public:
    void Fire()
    {
        std::function<void()> target;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_target)
            {
                m_pending = true;
                return;
            }
            target = m_target;
        }
        target();
    }

    void Bind(std::function<void()> target)
    {
        bool firePending = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_target = target;
            firePending = m_pending && m_target;
            if (firePending)
                m_pending = false;
        }
        if (firePending)
            target();
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_target = nullptr;
        m_pending = false;
    }

private:
    std::mutex m_mutex;
    std::function<void()> m_target;
    bool m_pending = false;
};

} // namespace nsk
