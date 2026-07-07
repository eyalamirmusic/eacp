#pragma once

#include "../Graphics/Keyboard.h"
#include <eacp/Core/Utils/Containers.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
    static MenuItem withSubmenu(std::string title, Menu menu);

    std::string title;
    MenuAction action = [] {};
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

} // namespace eacp::Graphics
