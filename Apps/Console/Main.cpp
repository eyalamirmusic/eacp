#include <eacp/Core/Core.h>
#include <eacp/Graphics/Graphics.h>

struct App
{
    App() {}

    void update()
    {
        for (auto& key: eacp::Graphics::Keyboard::getPressedKeys())
        {
            eacp::LOG(key.character);
        }
    }

    eacp::Threads::Timer timer {[&] { update(); }, 60};
};

int main()
{
    eacp::Apps::run<App>();
    return 0;
}