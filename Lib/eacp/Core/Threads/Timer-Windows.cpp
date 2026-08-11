#include "../Utils/WinInclude.h"

#include "Timer.h"
#include "../Utils/Singleton.h"
#include "ThreadUtils-Windows.h"

#include <cassert>
#include <unordered_map>

namespace eacp::Threads
{

// SetTimer with a null HWND posts WM_TIMER to the thread queue, so the callback
// rides the pump the event loop already runs, including inside the OS modal
// resize/move loop. Frame-accurate ticks want DisplayLink instead.
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
    // WM_TIMER carries nothing but the id, so the id is the only way back.
    using Registry = std::unordered_map<UINT_PTR, Native*>;

    // Immortal because a Timer owned by a singleton is destroyed during static
    // destruction, when an ordinary registry would already be gone.
    static Registry& liveTimers() { return Singleton::getImmortal<Registry>(); }

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
