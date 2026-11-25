#include <eacp/Core/Core.h>

using namespace eacp;

// callAsync, callAfter and a Timer on the page's own loop, in that order,
// then quit(0); quit(1) if they come out of order or not at all.
struct HelloLoop
{
    HelloLoop()
    {
        Console::print(Strings::concat(
            "HelloLoop on ", Platform::name(), ", ", Files::executablePath().str()));

        Threads::callAfter(Time::MS {50}, [this] { step("callAfter", 1); });
        Threads::callAsync([this] { step("callAsync", 0); });
        Threads::callAfter(Time::MS {5000}, [this] { finish(false); });
    }

    void step(const std::string& name, int expected)
    {
        Console::print(name);

        if (steps++ != expected)
            return finish(false);

        if (name == "callAfter")
            timer.emplace([this] { tick(); }, Time::MS {20});
    }

    void tick()
    {
        Console::print(Strings::concat("Timer tick ", ++ticks));

        if (ticks == 3)
            finish(steps == 2);
    }

    void finish(bool passed)
    {
        timer.reset();
        Console::print(passed ? "PASS" : "FAIL");
        Apps::quit(passed ? 0 : 1);
    }

    int steps = 0;
    int ticks = 0;
    std::optional<Threads::Timer> timer;
};

int main()
{
    return Apps::run<HelloLoop>();
}
