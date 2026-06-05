#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace Graphics;

// A smooth orange disc, generated so the example needs no asset file. On
// macOS the menu bar renders it as a template (alpha-only, system tinted);
// on Windows the colour shows in the notification area.
static Image makeTrayIcon()
{
    constexpr int size = 36;
    auto image = Image(size, size);

    auto center = (size - 1) / 2.f;
    auto radius = size * 0.42f;

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
        {
            auto dx = static_cast<float>(x) - center;
            auto dy = static_cast<float>(y) - center;
            auto distance = std::sqrt(dx * dx + dy * dy);

            auto coverage = std::clamp(radius - distance, 0.f, 1.f);
            if (coverage <= 0.f)
                continue;

            image.set(x, y, Color(0.95f, 0.55f, 0.1f, coverage));
        }
    }

    return image;
}

static Menu createTrayMenu()
{
    auto menu = Menu();
    menu.add(MenuItem::withAction("Say Hello", [] { LOG("Hello from the tray!"); }));
    menu.addSeparator();
    menu.add(MenuItem::withAction("Quit", [] { Apps::quit(); }));
    return menu;
}

struct TrayApp
{
    TrayApp()
    {
        Apps::setDockIconVisible(false);

        tray.setIcon(makeTrayIcon());
        tray.setTooltip("eacp Tray App");

        tray.setMenu(createTrayMenu());
    }

    TrayIcon tray;
};

int main()
{
    eacp::Apps::run<TrayApp>();
    return 0;
}
