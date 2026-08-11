#include "MenuCommands.h"

#include <cctype>

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

std::string keyText(const std::string& key)
{
    if (key.empty())
        return {};

    // "Ctrl+S", not "Ctrl+s". Longer names already read correctly, and
    // uppercasing them would give "ESCAPE".
    if (key.size() == 1)
        return std::string {(char) std::toupper((unsigned char) key[0])};

    auto text = key;
    text[0] = (char) std::toupper((unsigned char) text[0]);

    return text;
}
} // namespace

MenuEntryKind classifyMenuEntry(const MenuItem& item)
{
    // Separator first, so an item carrying both flags resolves one way only.
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

    // Ctrl, Alt, Shift is the order Windows itself prints.
    auto text = std::string {};

    // Control and command both mean the primary accelerator modifier here, so
    // they collapse to one "Ctrl+".
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
} // namespace eacp::Graphics
