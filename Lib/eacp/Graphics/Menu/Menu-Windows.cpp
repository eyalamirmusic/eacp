#include "Menu.h"

#include "../Helpers/StringUtils-Windows.h"
#include "../Window/Window.h"
#include "MenuCommands.h"
#include "Win32Menu.h"

#include <unordered_map>

namespace eacp::Graphics
{
namespace
{
// One window's menu bar: the native handle, and the table that turns a
// WM_COMMAND id back into something to run.
//
// Keyed by HWND rather than held on the Window, because the messages that need
// it arrive at the WndProc with nothing but the handle.
struct InstalledBar
{
    HMENU menu = nullptr;
    Vector<MenuCommand> commands;
};

// Deliberately leaked. A Window at namespace scope is constructed before this
// static and so destroyed after it, and ~Native() reaches in here to remove its
// bar — which would touch a destroyed map. Leaking one hash table at exit is
// the cheaper end of that trade.
std::unordered_map<HWND, InstalledBar>& installedBars()
{
    static auto* bars = new std::unordered_map<HWND, InstalledBar>();
    return *bars;
}

void appendItem(HMENU parent, const MenuItem& item, unsigned& nextId);

HMENU buildPopup(const Menu& menu, unsigned& nextId)
{
    auto* popup = CreatePopupMenu();

    for (const auto& item: menu.items)
        appendItem(popup, item, nextId);

    return popup;
}

// AppendMenuW reads '&' as the mnemonic prefix, so a title containing one draws
// as an underscore and eats the next character — "Find & Replace" becomes
// "Find _Replace". Doubling escapes it.
//
// Done here rather than in menuItemLabel because it is this API's convention,
// not the model's: macOS wants the raw string, and the same MenuBar is built
// for both.
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
    // Switches on the shared classification rather than asking its own
    // questions, so its ids cannot drift out of step with flattenCommands.
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

        // Present in the model, absent from this backend. Skipped rather than
        // added dead — and flattenCommands skips it too.
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

    // Both this walk and flattenCommands assign ids by switching on
    // classifyMenuEntry, so they agree structurally rather than by two
    // functions having been written to match. MenuCommandsTests covers that
    // classification, which is the half of the agreement a Mac can check.
    auto nextId = 1u;

    for (const auto& menu: bar.menus)
    {
        auto* popup = buildPopup(menu, nextId);

        // A menu that came out empty is left off the bar entirely. macOS's
        // standardApplicationMenu is empty here by design — Windows has no
        // About/Hide/Quit block — and buildDefaultWebViewMenuBar adds it
        // regardless, so without this five shipped examples grow a top-level
        // title that opens onto nothing.
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

    // The window was sized before it had a menu — AdjustWindowRectExForDpi was
    // told bMenu = FALSE, because at creation time it was true — and SetMenu
    // takes the bar's height straight out of the client area. So the content
    // would silently come up a menu-bar shorter than the size it asked for.
    // Measured rather than calculated: the bar can wrap to two rows on a narrow
    // window, and GetSystemMetrics(SM_CYMENU) only ever describes one.
    RECT client {};
    GetClientRect(hwnd, &client);

    const auto heightBefore = client.bottom - client.top;

    SetMenu(hwnd, installed.menu);

    // The bar is non-client area, so the window has to be told to redraw it:
    // without this the menu exists and simply is not painted until something
    // else forces a frame.
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

    // Copied before running: the action may install a new menu bar, which would
    // free the table this pointer is into while it is still being called.
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

    // Copied before asking, for the same reason handleWin32MenuCommand copies
    // the action: a predicate that reinstalls the menu bar would rehash the map
    // and free this vector while it is still being walked.
    auto* menu = found->second.menu;
    const auto commands = found->second.commands;

    // MF_BYCOMMAND searches the whole tree from the root, so every item can be
    // updated from the top without tracking which popup is opening.
    for (const auto& command: commands)
    {
        const auto enabled = command.isEnabled && command.isEnabled();

        EnableMenuItem(
            menu, command.id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
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
    // Windows has no application menu bar — a menu belongs to a window and is
    // drawn inside its frame. An app with several windows installs one per
    // window; on macOS the same call installs it once and ignores which window
    // was passed.
    //
    // Worth knowing about the accelerator column this prints: Win32 menu
    // accelerators are decorative text and nothing more, so an item reading
    // "Ctrl+S" does not make Ctrl+S work. The keystroke still has to arrive
    // through the application's own keymap — and eacp's Keyboard reports
    // `command` for the *Windows* key here, so an app binding cmd+S prints
    // Ctrl+S and responds to Win+S until that mapping is settled. See
    // acceleratorText in MenuCommands.h.
    detail::installWin32MenuBar(static_cast<HWND>(window.getHandle()), bar);
}

Menu standardApplicationMenu(std::string applicationName)
{
    // Windows puts no About/Hide/Quit block in a menu bar — the window's system
    // menu and close button do that job — so this stays an empty menu carrying
    // the name, and an app is expected to leave it out. Kept rather than
    // removed so portable code can build one bar for both platforms.
    return Menu {std::move(applicationName)};
}

Menu standardEditMenu()
{
    auto menu = Menu {"Edit"};

    // The macOS version routes these down the responder chain with selectors,
    // which Windows has no equivalent for — so nothing here can actually run.
    //
    // Greyed rather than left looking available: an item that draws enabled,
    // prints "Ctrl+V" and silently does nothing on click is worse than one that
    // visibly cannot be used. An app wanting working entries builds them from
    // its own commands, which is what ECode does.
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
