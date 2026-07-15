#pragma once

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{
using MenuAction = std::function<void()>;

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
        std::optional<KeyEquivalent> shortcut = {});

    // An item that sends an Objective-C selector ("copy:") to whatever
    // currently has focus, rather than running a C++ action. The focused view
    // implements the command, so the item works for any WebView or text field
    // and greys itself out when that view can't perform it. Ignored on the
    // platforms with no menu bar.
    static MenuItem withResponderSelector(
        std::string title,
        std::string selector,
        std::optional<KeyEquivalent> shortcut = {});

    static MenuItem withSubmenu(std::string title, Menu menu);

    std::string title;
    MenuAction action = [] {};
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

void setApplicationMenuBar(const MenuBar& bar);

Menu standardApplicationMenu(std::string applicationName);

// The standard Edit menu. An app that hosts a WebView needs this in its menu
// bar to get Cmd+X/C/V/A at all: macOS delivers those to the focused view by
// matching them against the menu bar, never as plain key-down events.
Menu standardEditMenu();

} // namespace eacp::Graphics
