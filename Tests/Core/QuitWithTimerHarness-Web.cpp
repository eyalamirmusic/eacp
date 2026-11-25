#include <eacp/Core/Core.h>

using namespace eacp;

namespace
{
Threads::Timer& staticTimer()
{
    static auto timer = Threads::Timer {[] {}, Time::MS {20}};
    return timer;
}
} // namespace

struct QuitWithTimerHarness
{
    QuitWithTimerHarness()
    {
        staticTimer();
        Threads::callAfter(Time::MS {100}, [] { Apps::quit(0); });
    }
};

int main()
{
    return Apps::run<QuitWithTimerHarness>();
}
