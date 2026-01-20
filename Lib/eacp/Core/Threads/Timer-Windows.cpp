#include "Timer.h"
#include "ThreadUtils.h"
#include "EventLoop.h"

#include <unordered_map>
#include "ThreadUtils-Windows.h"

namespace eacp::Threads
{

struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : cb(cbToUse)
    {
        assertMainThread();
        UINT intervalMs = 1000 / intervalHz;

        timerId = SetTimer(nullptr, 0, intervalMs, &timerProc);
        getTimerMap()[timerId] = this;
    }

    ~Native()
    {
        assertMainThread();
        KillTimer(nullptr, timerId);
        getTimerMap().erase(timerId);
    }

    static void CALLBACK timerProc(HWND, UINT, UINT_PTR idEvent, DWORD)
    {
        auto& map = getTimerMap();
        auto it = map.find(idEvent);

        if (it != map.end())
        {
            it->second->cb();
        }
    }

    using NativeMap = std::unordered_map<UINT_PTR, Native*>;

    static NativeMap& getTimerMap()
    {
        static auto map = NativeMap();
        return map;
    }

    Callback cb;
    UINT_PTR timerId = 0;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

} // namespace eacp::Threads
