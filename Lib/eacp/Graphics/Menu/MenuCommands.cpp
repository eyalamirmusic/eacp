#include "MenuCommands.h"

#include <eacp/Core/Utils/Strings.h>

namespace eacp::Graphics
{
namespace
{
void collect(const Menu& menu, Vector<MenuCommand>& out, unsigned& nextId)
{
    for (const auto& item: menu.items)
    {
        switch (classifyMenuEntry(item))
        {
            case MenuEntryKind::Separator:
            case MenuEntryKind::Skipped:
                continue;

            case MenuEntryKind::Submenu:
                collect(*item.submenu, out, nextId);
                continue;

            case MenuEntryKind::Command:
                break;
        }

        auto command = MenuCommand {};
        command.id = nextId++;
        command.action = item.action;
        command.isEnabled = item.isEnabled;
        command.isChecked = item.isChecked;

        out.add(std::move(command));
    }
}

const MenuItem* findShortcutIn(const Menu& menu, const KeyEvent& event)
{
    for (const auto& item: menu.items)
    {
        switch (classifyMenuEntry(item))
        {
            case MenuEntryKind::Separator:
            case MenuEntryKind::Skipped:
                continue;

            case MenuEntryKind::Submenu:
                if (const auto* found = findShortcutIn(*item.submenu, event))
                    return found;

                continue;

            case MenuEntryKind::Command:
                break;
        }

        if (item.shortcut && matchesShortcut(*item.shortcut, event) && item.isEnabled
            && item.isEnabled())
            return &item;
    }

    return nullptr;
}

// False when nothing in it can be chosen: empty, or only separators and
// submenus that are themselves empty.
bool keepChoosableItems(Menu& menu)
{
    auto kept = Vector<MenuItem> {};
    auto anyChoosable = false;

    for (auto& item: menu.items)
    {
        switch (classifyMenuEntry(item))
        {
            case MenuEntryKind::Skipped:
                continue;

            case MenuEntryKind::Submenu:
            {
                auto submenu = *item.submenu;

                if (!keepChoosableItems(submenu))
                    continue;

                item.submenu = std::make_shared<Menu>(std::move(submenu));
                anyChoosable = true;
                break;
            }

            case MenuEntryKind::Command:
                anyChoosable = true;
                break;

            case MenuEntryKind::Separator:
                break;
        }

        kept.add(item);
    }

    menu.items = std::move(kept);
    return anyChoosable;
}

std::string keyText(const std::string& key)
{
    // A single character is a letter or punctuation and reads capitalised —
    // "Ctrl+S", not "Ctrl+s". Anything longer already names itself ("Escape",
    // "F5") and keeps the rest of its casing, because uppercasing the whole
    // thing would give "ESCAPE".
    return Strings::capitalize(key);
}
} // namespace

MenuEntryKind classifyMenuEntry(const MenuItem& item)
{
    // Separator first, because an item carrying both flags has to resolve one
    // way and only one — which walk asked first is exactly what used to differ.
    if (item.isSeparator)
        return MenuEntryKind::Separator;

    if (item.submenu)
        return MenuEntryKind::Submenu;

    if (!item.responderSelector.empty())
        return MenuEntryKind::Skipped;

    return MenuEntryKind::Command;
}

Vector<MenuCommand> flattenCommands(const MenuBar& bar, unsigned firstId)
{
    auto commands = Vector<MenuCommand> {};
    auto nextId = firstId;

    for (const auto& menu: bar.menus)
        collect(menu, commands, nextId);

    return commands;
}

const MenuCommand* findCommand(const Vector<MenuCommand>& commands, unsigned id)
{
    for (const auto& command: commands)
        if (command.id == id)
            return &command;

    return nullptr;
}

std::string acceleratorText(const KeyEquivalent& shortcut)
{
    const auto key = keyText(shortcut.key);

    if (key.empty())
        return {};

    // Ctrl, Alt, Shift — the order Windows itself prints, so the column reads
    // the same as every other application's.
    auto text = std::string {};

    // Both spellings collapse to one "Ctrl+". A chord that set control *and*
    // command would otherwise print "Ctrl+Ctrl+S", and the two mean the same
    // thing here — the primary accelerator modifier.
    if (shortcut.modifiers.control || shortcut.modifiers.command)
        text += "Ctrl+";

    if (shortcut.modifiers.alt)
        text += "Alt+";

    if (shortcut.modifiers.shift)
        text += "Shift+";

    return text + key;
}

std::string menuItemLabel(const MenuItem& item)
{
    const auto accelerator =
        item.shortcut ? acceleratorText(*item.shortcut) : std::string {};

    if (accelerator.empty())
        return item.title;

    return item.title + "\t" + accelerator;
}

bool matchesShortcut(const KeyEquivalent& shortcut, const KeyEvent& event)
{
    const auto& wanted = shortcut.modifiers;
    const auto& held = event.modifiers;

    if (held.control != (wanted.control || wanted.command) || held.command
        || held.alt != wanted.alt || held.shift != wanted.shift)
        return false;

    return shortcut.key.size() == 1
           && Strings::toLower(event.charactersIgnoringModifiers)
                  == Strings::toLower(shortcut.key);
}

const MenuItem* findShortcutItem(const MenuBar& bar, const KeyEvent& event)
{
    if (event.type != KeyEventType::Down)
        return nullptr;

    for (const auto& menu: bar.menus)
        if (const auto* found = findShortcutIn(menu, event))
            return found;

    return nullptr;
}

MenuBar visibleMenuBar(const MenuBar& bar)
{
    auto visible = MenuBar {};

    for (auto menu: bar.menus)
        if (keepChoosableItems(menu))
            visible.add(std::move(menu));

    return visible;
}
} // namespace eacp::Graphics
