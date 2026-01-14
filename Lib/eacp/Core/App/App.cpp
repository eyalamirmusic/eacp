#include "App.h"
#include "../Utils/Singleton.h"

namespace eacp::Apps
{
AppHandle& getGlobalApp()
{
    return Singleton::get<AppHandle>();
}

void destroyApp()
{
    getGlobalApp().reset();
}

void quit()
{
    auto quitFunc = []
    {
        destroyApp();
        Threads::stopEventLoop();
    };

    Threads::callAsync(quitFunc);
}
} // namespace eacp::Apps