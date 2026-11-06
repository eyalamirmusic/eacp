#pragma once

/** @file The portable menu model (Menu, MenuItem, MenuBar) and installing it as a
    window's native menu bar. */

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{
/// Runs on the main thread when the item is chosen.
using MenuAction = std::function<void()>;

/// Asked each time the menu is about to be shown, so an item greys itself out
/// from live state rather than the application rebuilding the bar whenever
/// something changes. Ignored on the platforms with no menu bar.
using MenuEnabled = std::function<bool()>;

/// Asked at the same moment as MenuEnabled, and for the same reason: the
/// checkmark follows live state instead of being sampled when the bar was
/// built. Null (the default) means the item is not checkable at all — it never
/// shows a mark — which is distinct from a predicate returning false, where the
/// item participates in checking and is currently off. Ignored on the platforms
/// with no menu bar.
using MenuChecked = std::function<bool()>;

/// A menu shortcut: the key's character ("s") plus modifiers.
struct KeyEquivalent
{
    std::string key;
    ModifierKeys modifiers;
};

/// Cmd+key on macOS; rendered as Ctrl+key in a Windows menu label.
KeyEquivalent commandKey(std::string key);
KeyEquivalent commandShiftKey(std::string key);
KeyEquivalent commandAltKey(std::string key);

class Menu;

/// One entry of a Menu; build it with the static factories.
struct MenuItem
{
    static MenuItem separator();
    static MenuItem withAction(
        std::string title,
        MenuAction action = [] {},
        std::optional<KeyEquivalent> shortcut = {},
        MenuEnabled isEnabled = [] { return true; });

    /// An item that sends an Objective-C selector ("copy:") to whatever
    /// currently has focus, rather than running a C++ action. The focused view
    /// implements the command, so the item works for any WebView or text field
    /// and greys itself out when that view can't perform it. Ignored on the
    /// platforms with no menu bar.
    static MenuItem
        withResponderSelector(std::string title,
                              std::string selector,
                              std::optional<KeyEquivalent> shortcut = {});

    /// An item that shows a checkmark when `isChecked` returns true — one
    /// entry in a pick-one group (a radio list like a device picker) or a
    /// standalone toggle. The action still does the flipping; the predicate
    /// only reports.
    static MenuItem withCheckableAction(
        std::string title,
        MenuAction action,
        MenuChecked isChecked,
        std::optional<KeyEquivalent> shortcut = {},
        MenuEnabled isEnabled = [] { return true; });

    static MenuItem withSubmenu(std::string title, Menu menu);

    std::string title;
    MenuAction action = [] {};

    /// Non-null by default, so an item that says nothing about availability is
    /// always available and every call site can invoke this without a check.
    /// Not consulted for a responder-selector item: there the focused view
    /// answers, which is the whole point of routing through the chain.
    MenuEnabled isEnabled = [] { return true; };

    /// Null by default — see MenuChecked: null is "not checkable", not
    /// "unchecked", so every backend must check before calling.
    MenuChecked isChecked;

    std::string responderSelector;
    std::optional<KeyEquivalent> shortcut;
    std::shared_ptr<Menu> submenu;
    bool isSeparator = false;
};

/// A titled list of items: one top-level menu or a submenu.
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

/// The top-level menus, left to right.
class MenuBar
{
public:
    MenuBar& add(Menu menu);

    Vector<Menu> menus;
};

class Window;

/// Installs `bar` as the menu bar for `window`.
///
/// The window argument is what makes this implementable at all off macOS. A
/// macOS menu bar belongs to the *application* and is shown for whichever window
/// is active, so the argument is ignored there. Windows has no application menu
/// bar: a menu is owned by an HWND and drawn inside that window's frame, so it
/// has to be told which one. An app with several windows calls this per window
/// on Windows and once on macOS — passing any of them.
///
/// Ignored on iOS and Android, which have no menu bar, and on Linux.
void setApplicationMenuBar(const MenuBar& bar, Window& window);

/// The macOS application menu. Elsewhere an empty menu with that title.
Menu standardApplicationMenu(std::string applicationName);

/// The standard Edit menu. An app that hosts a WebView needs this in its menu
/// bar to get Cmd+X/C/V/A at all: macOS delivers those to the focused view by
/// matching them against the menu bar, never as plain key-down events.
Menu standardEditMenu();

} // namespace eacp::Graphics
