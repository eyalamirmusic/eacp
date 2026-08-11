#include "Menu.h"

#include "../Helpers/StringUtils-Windows.h"
#include "../Window/Window.h"
#include "MenuCommands.h"
#include "Win32Menu.h"

#include <eacp/Core/Utils/Singleton.h>

#include <unordered_map>

namespace eacp::Graphics
{
namespace
{
// Keyed by HWND rather than held on the Window, because the messages needing
// it arrive at the WndProc with nothing but the handle.
struct InstalledBar
{
    HMENU menu = nullptr;
    Vector<MenuCommand> commands;
};

// Immortal because a Window at namespace scope can outlive it and ~Native()
// reaches in here to remove its bar.
std::unordered_map<HWND, InstalledBar>& installedBars()
{
    return Singleton::getImmortal<std::unordered_map<HWND, InstalledBar>>();
}

void appendItem(HMENU parent, const MenuItem& item, unsigned& nextId);

HMENU buildPopup(const Menu& menu, unsigned& nextId)
{
    auto* popup = CreatePopupMenu();

    for (const auto& item: menu.items)
        appendItem(popup, item, nextId);

    return popup;
}

// AppendMenuW reads '&' as the mnemonic prefix; doubling escapes it. Done here
// rather than in menuItemLabel because macOS wants the raw string.
std::wstring toMenuText(const std::string& text)
{
    auto escaped = std::string {};
    escaped.reserve(text.size());

    for (const auto character: text)
    {
        escaped += character;

        if (character == '&')
            escaped += '&';
    }

    return toWideString(escaped);
}

void appendItem(HMENU parent, const MenuItem& item, unsigned& nextId)
{
    // Shares classifyMenuEntry with flattenCommands so ids cannot drift.
    switch (classifyMenuEntry(item))
    {
        case MenuEntryKind::Separator:
            AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
            return;

        case MenuEntryKind::Submenu:
        {
            auto* submenu = buildPopup(*item.submenu, nextId);

            AppendMenuW(parent,
                        MF_POPUP,
                        reinterpret_cast<UINT_PTR>(submenu),
                        toMenuText(item.title).c_str());
            return;
        }

        case MenuEntryKind::Skipped:
            return;

        case MenuEntryKind::Command:
            break;
    }

    AppendMenuW(
        parent, MF_STRING, nextId++, toMenuText(menuItemLabel(item)).c_str());
}
} // namespace

namespace detail
{
void installWin32MenuBar(HWND hwnd, const MenuBar& bar)
{
    if (hwnd == nullptr)
        return;

    removeWin32MenuBar(hwnd);

    auto installed = InstalledBar {};
    installed.menu = CreateMenu();

    auto nextId = 1u;

    for (const auto& menu: bar.menus)
    {
        auto* popup = buildPopup(menu, nextId);

        // An empty menu is left off the bar rather than opening onto nothing;
        // standardApplicationMenu is empty on Windows by design.
        if (GetMenuItemCount(popup) <= 0)
        {
            DestroyMenu(popup);
            continue;
        }

        AppendMenuW(installed.menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(popup),
                    toMenuText(menu.title).c_str());
    }

    installed.commands = flattenCommands(bar);

    // SetMenu takes the bar's height out of the client area, so measure and
    // give it back. Measured, not calculated: the bar can wrap to two rows.
    RECT client {};
    GetClientRect(hwnd, &client);

    const auto heightBefore = client.bottom - client.top;

    SetMenu(hwnd, installed.menu);

    // The bar is non-client area, so it is not painted until asked.
    DrawMenuBar(hwnd);

    GetClientRect(hwnd, &client);

    if (const auto lost = heightBefore - (client.bottom - client.top); lost > 0)
    {
        RECT frame {};
        GetWindowRect(hwnd, &frame);

        SetWindowPos(hwnd,
                     nullptr,
                     0,
                     0,
                     frame.right - frame.left,
                     (frame.bottom - frame.top) + lost,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    installedBars()[hwnd] = std::move(installed);
}

bool handleWin32MenuCommand(HWND hwnd, unsigned id)
{
    auto& bars = installedBars();
    const auto found = bars.find(hwnd);

    if (found == bars.end())
        return false;

    const auto* command = findCommand(found->second.commands, id);

    if (command == nullptr)
        return false;

    // Copied: the action may install a new bar, freeing the table it points to.
    const auto action = command->action;

    if (action)
        action();

    return true;
}

void updateWin32MenuEnabledState(HWND hwnd)
{
    auto& bars = installedBars();
    const auto found = bars.find(hwnd);

    if (found == bars.end())
        return;

    // Copied: a predicate that reinstalls the bar would free this vector.
    auto* menu = found->second.menu;
    const auto commands = found->second.commands;

    // MF_BYCOMMAND searches the whole tree, so the opening popup is irrelevant.
    for (const auto& command: commands)
    {
        const auto enabled = command.isEnabled && command.isEnabled();

        EnableMenuItem(
            menu, command.id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));

        // A command with no predicate is left alone rather than unchecked.
        if (command.isChecked)
            CheckMenuItem(menu,
                          command.id,
                          MF_BYCOMMAND
                              | (command.isChecked() ? MF_CHECKED : MF_UNCHECKED));
    }
}

void removeWin32MenuBar(HWND hwnd)
{
    auto& bars = installedBars();
    const auto found = bars.find(hwnd);

    if (found == bars.end())
        return;

    // DestroyMenu frees the whole tree, submenus included.
    if (found->second.menu != nullptr)
    {
        SetMenu(hwnd, nullptr);
        DestroyMenu(found->second.menu);
    }

    bars.erase(found);
}
} // namespace detail

void setApplicationMenuBar(const MenuBar& bar, Window& window)
{
    // Windows menus belong to a window, so an app with several installs one
    // per window. Win32 menu accelerators are decorative text only: the
    // keystroke still has to arrive through the app's own keymap.
    detail::installWin32MenuBar(static_cast<HWND>(window.getHandle()), bar);
}

Menu standardApplicationMenu(std::string applicationName)
{
    // Windows puts no About/Hide/Quit block in a menu bar; kept empty so
    // portable code can build one bar for both platforms.
    return Menu {std::move(applicationName)};
}

Menu standardEditMenu()
{
    auto menu = Menu {"Edit"};

    // Windows has no responder chain, so these cannot run; greyed rather than
    // left looking available. Apps build working entries from their own
    // commands.
    const auto unavailable = [] { return false; };

    menu.add(MenuItem::withAction("Undo", [] {}, commandKey("z"), unavailable));
    menu.add(MenuItem::withAction("Redo", [] {}, commandShiftKey("z"), unavailable));

    menu.addSeparator();

    menu.add(MenuItem::withAction("Cut", [] {}, commandKey("x"), unavailable));
    menu.add(MenuItem::withAction("Copy", [] {}, commandKey("c"), unavailable));
    menu.add(MenuItem::withAction("Paste", [] {}, commandKey("v"), unavailable));

    menu.addSeparator();

    menu.add(
        MenuItem::withAction("Select All", [] {}, commandKey("a"), unavailable));

    return menu;
}
} // namespace eacp::Graphics
