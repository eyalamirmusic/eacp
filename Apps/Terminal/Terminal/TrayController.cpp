#include "TrayController.h"
#include "DaemonClient.h"
#include "Notifier.h"

#include <eacp/Core/App/App.h>

namespace term
{
using namespace eacp;
using Graphics::Image;
using Graphics::Menu;
using Graphics::MenuItem;

// A little terminal glyph, drawn as alpha only so the menu bar tints it
// (template rendering): a frame, a prompt chevron and a cursor underscore.
Image TrayController::makeIcon()
{
    constexpr auto size = 36;
    auto image = Image {size, size};
    const auto on = Graphics::Color::white();

    auto block = [&](int x, int y, int w, int h)
    {
        for (auto py = y; py < y + h; ++py)
            for (auto px = x; px < x + w; ++px)
                image.set(px, py, on);
    };

    block(2, 5, 32, 2);
    block(2, 29, 32, 2);
    block(2, 5, 2, 26);
    block(32, 5, 2, 26);

    for (auto i = 0; i < 5; ++i)
    {
        block(8 + i, 12 + i, 2, 2);
        block(8 + i, 20 - i, 2, 2);
    }

    block(17, 23, 10, 2);

    return image;
}

TrayController::TrayController(SessionManager& sessionsToUse)
    : sessions(sessionsToUse)
{
    icon.setIcon(makeIcon());
    icon.setTooltip("wim terminal");
    Notifier::attachTray(icon);
    refresh();
}

void TrayController::refresh()
{
    auto menu = Menu {};

    menu.add(MenuItem::withAction("Show wim terminal", [this] { onShowWindow(); }));
    menu.addSeparator();

    for (auto& session: sessions.all())
    {
        auto* raw = session.get();
        auto title = std::string {};

        if (sessions.active() == raw)
            title += "● ";

        if (raw->isClaude())
            title += "✳ ";

        title += raw->name;

        if (!raw->lastNotify.empty())
            title += " — " + raw->lastNotify;

        menu.add(MenuItem::withAction(title, [this, raw] { onPickSession(*raw); }));
    }

    menu.addSeparator();

    const auto haveDaemon =
        DaemonClient::get() != nullptr && DaemonClient::get()->isConnected();

    menu.add(MenuItem::withAction(haveDaemon ? "Quit (shells keep running)" : "Quit",
                                  [] { Apps::quit(); }));

    if (haveDaemon)
        menu.add(MenuItem::withAction("Kill everything & quit",
                                      []
                                      {
                                          if (auto* client = DaemonClient::get())
                                              client->killServer();

                                          Apps::quit();
                                      }));

    icon.setMenu(menu);
}
} // namespace term
