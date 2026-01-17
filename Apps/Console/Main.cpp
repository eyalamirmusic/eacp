#include <eacp/Core/Core.h>
#include <eacp/Graphics/Graphics.h>

struct App
{
    void update()
    {
        eacp::LOG(std::to_string(numTimes));

        numTimes++;

        if (numTimes == 5)
            eacp::Apps::quit();
    }

    int numTimes = 0;
    eacp::Threads::Timer timer {[&] { update(); }, 1};
};

int main()
{
    eacp::Apps::run<App>();
    return 0;
}