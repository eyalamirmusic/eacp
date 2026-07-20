#include "Menu.h"

namespace eacp::Graphics
{
namespace
{
MenuAction safeAction(MenuAction action)
{
    if (action)
        return action;

    return [] {};
}

MenuEnabled safeEnabled(MenuEnabled isEnabled)
{
    if (isEnabled)
        return isEnabled;

    return [] { return true; };
}
} // namespace

KeyEquivalent commandKey(std::string key)
{
    auto eq = KeyEquivalent {};
    eq.key = std::move(key);
    eq.modifiers.command = true;
    return eq;
}

KeyEquivalent commandShiftKey(std::string key)
{
    auto eq = commandKey(std::move(key));
    eq.modifiers.shift = true;
    return eq;
}

KeyEquivalent commandAltKey(std::string key)
{
    auto eq = commandKey(std::move(key));
    eq.modifiers.alt = true;
    return eq;
}

MenuItem MenuItem::separator()
{
    auto item = MenuItem {};
    item.isSeparator = true;
    return item;
}

MenuItem MenuItem::withAction(std::string title,
                              MenuAction action,
                              std::optional<KeyEquivalent> shortcut,
                              MenuEnabled isEnabled)
{
    auto item = MenuItem {};
    item.title = std::move(title);
    item.action = safeAction(std::move(action));
    item.isEnabled = safeEnabled(std::move(isEnabled));
    item.shortcut = std::move(shortcut);
    return item;
}

MenuItem MenuItem::withCheckableAction(std::string title,
                                       MenuAction action,
                                       MenuChecked isChecked,
                                       std::optional<KeyEquivalent> shortcut,
                                       MenuEnabled isEnabled)
{
    auto item = withAction(std::move(title),
                           std::move(action),
                           std::move(shortcut),
                           std::move(isEnabled));
    item.isChecked = std::move(isChecked);
    return item;
}

MenuItem MenuItem::withResponderSelector(std::string title,
                                         std::string selector,
                                         std::optional<KeyEquivalent> shortcut)
{
    auto item = MenuItem {};
    item.title = std::move(title);
    item.responderSelector = std::move(selector);
    item.shortcut = std::move(shortcut);
    return item;
}

MenuItem MenuItem::withSubmenu(std::string title, Menu menu)
{
    auto item = MenuItem {};
    item.title = std::move(title);
    item.submenu = std::make_shared<Menu>(std::move(menu));
    return item;
}

Menu::Menu(std::string titleToUse)
    : title(std::move(titleToUse))
{
}

Menu& Menu::add(MenuItem item)
{
    item.action = safeAction(std::move(item.action));
    item.isEnabled = safeEnabled(std::move(item.isEnabled));
    items.add(std::move(item));
    return *this;
}

Menu& Menu::addSeparator()
{
    items.add(MenuItem::separator());
    return *this;
}

MenuBar& MenuBar::add(Menu menu)
{
    menus.add(std::move(menu));
    return *this;
}

} // namespace eacp::Graphics
