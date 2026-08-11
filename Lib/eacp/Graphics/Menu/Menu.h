#pragma once

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{
using MenuAction = std::function<void()>;

// Asked each time the menu is about to be shown, so an item greys itself from
// live state. Ignored on the platforms with no menu bar.
using MenuEnabled = std::function<bool()>;

// Asked at the same moment as MenuEnabled. Null (the default) means "not
// checkable at all", distinct from a predicate returning false.
using MenuChecked = std::function<bool()>;

struct KeyEquivalent
{
    std::string key;
    ModifierKeys modifiers;
};

KeyEquivalent commandKey(std::string key);
KeyEquivalent commandShiftKey(std::string key);
KeyEquivalent commandAltKey(std::string key);

class Menu;

struct MenuItem
{
    static MenuItem separator();
    static MenuItem withAction(
        std::string title,
        MenuAction action = [] {},
        std::optional<KeyEquivalent> shortcut = {},
        MenuEnabled isEnabled = [] { return true; });

    // Sends an Objective-C selector ("copy:") to whatever has focus instead of
    // running a C++ action. Ignored on the platforms with no menu bar.
    static MenuItem
        withResponderSelector(std::string title,
                              std::string selector,
                              std::optional<KeyEquivalent> shortcut = {});

    // The action still does the flipping; `isChecked` only reports.
    static MenuItem withCheckableAction(
        std::string title,
        MenuAction action,
        MenuChecked isChecked,
        std::optional<KeyEquivalent> shortcut = {},
        MenuEnabled isEnabled = [] { return true; });

    static MenuItem withSubmenu(std::string title, Menu menu);

    std::string title;
    MenuAction action = [] {};

    // Not consulted for a responder-selector item; the focused view answers.
    MenuEnabled isEnabled = [] { return true; };

    // Null by default: "not checkable", not "unchecked". Check before calling.
    MenuChecked isChecked;

    std::string responderSelector;
    std::optional<KeyEquivalent> shortcut;
    std::shared_ptr<Menu> submenu;
    bool isSeparator = false;
};

class Menu
{
public:
    Menu() = default;
    explicit Menu(std::string title);

    Menu& add(MenuItem item);
    Menu& addSeparator();

    std::string title;
    Vector<MenuItem> items;
};

class MenuBar
{
public:
    MenuBar& add(Menu menu);

    Vector<Menu> menus;
};

class Window;

// A macOS menu bar belongs to the application, so `window` is ignored there;
// on Windows a menu is owned by an HWND, so call this once per window. No-op
// on iOS.
void setApplicationMenuBar(const MenuBar& bar, Window& window);

Menu standardApplicationMenu(std::string applicationName);

// A WebView host needs this in its bar to get Cmd+X/C/V/A at all: macOS only
// delivers those by matching them against the menu bar.
Menu standardEditMenu();

} // namespace eacp::Graphics
