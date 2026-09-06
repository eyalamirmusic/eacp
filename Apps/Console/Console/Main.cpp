#include <eacp/Core/Core.h>

using namespace eacp;

struct App
{
    void update()
    {
        LOG(numTimes);

        numTimes++;

        if (numTimes == 4)
            Apps::quit();
    }

    int numTimes = 0;
    Threads::Timer timer {[&] { update(); }, 1};
};

int main()
{
    return Apps::run<App>();
}