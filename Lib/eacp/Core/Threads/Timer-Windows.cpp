#include "../Utils/WinInclude.h"

#include "Timer.h"
#include "ThreadUtils-Windows.h"

#include <cassert>
#include <unordered_map>

namespace eacp::Threads
{

// A plain Win32 timer replaces the WinRT DispatcherQueueTimer. SetTimer with a
// null HWND posts WM_TIMER to the thread's queue and DispatchMessage invokes the
// callback, so it rides the same pump the event loop already runs — including
// inside the OS modal resize/move loop, which the dispatcher queue also managed.
// Callers wanting frame-accurate ticks should use DisplayLink, as before.
struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : cb(cbToUse)
    {
        assertMainThread();
        assert(intervalHz > 0 && "Timer interval must be positive");

        auto periodMs = static_cast<UINT>(1000.0 / intervalHz);

        if (periodMs == 0)
            periodMs = 1;

        id = SetTimer(nullptr, 0, periodMs, &Native::tick);

        if (id != 0)
            liveTimers()[id] = this;
    }

    ~Native()
    {
        assertMainThread();

        if (id != 0)
        {
            KillTimer(nullptr, id);
            liveTimers().erase(id);
            id = 0;
        }
    }

    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;

private:
    static std::unordered_map<UINT_PTR, Native*>& liveTimers()
    {
        static auto timers = std::unordered_map<UINT_PTR, Native*>();
        return timers;
    }

    static void CALLBACK tick(HWND, UINT, UINT_PTR timerId, DWORD)
    {
        auto& timers = liveTimers();
        auto entry = timers.find(timerId);

        if (entry != timers.end())
            entry->second->cb();
    }

    Callback cb;
    UINT_PTR id = 0;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

} // namespace eacp::Threads
