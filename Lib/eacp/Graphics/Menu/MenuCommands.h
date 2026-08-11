#pragma once

#include "Menu.h"

namespace eacp::Graphics
{
// The portable half of a menu bar that identifies its items by integer command
// id, as Win32 does and macOS does not. Compiled and tested on every platform.

// The single place deciding which items consume a command id: the Win32 builder
// and flattenCommands both switch on this, so their ids cannot drift apart.
enum class MenuEntryKind
{
    Separator,
    Submenu,
    Command,

    // Present in the model, absent from this backend (a responder-selector
    // item). Consumes no id.
    Skipped
};

// Separator wins over submenu, which wins over everything else.
MenuEntryKind classifyMenuEntry(const MenuItem& item);

struct MenuCommand
{
    unsigned id = 0;
    MenuAction action = [] {};
    MenuEnabled isEnabled = [] { return true; };

    // Null when the item is not checkable — see MenuChecked.
    MenuChecked isChecked;
};

// Every actionable item, depth-first in the order a builder appends them. Ids
// start at 1: zero means "nothing was chosen" to TrackPopupMenu and WM_COMMAND.
Vector<MenuCommand> flattenCommands(const MenuBar& bar, unsigned firstId = 1);

// Null when no command carries that id, which is the case for every WM_COMMAND
// that came from something other than this menu bar.
const MenuCommand* findCommand(const Vector<MenuCommand>& commands, unsigned id);

// "Ctrl+Shift+P" — decorative text only; the keystroke must still be delivered
// by the app's own keymap. `command` renders as "Ctrl".
std::string acceleratorText(const KeyEquivalent& shortcut);

// "Save\tCtrl+S", or just "Save" when there is no shortcut. The tab is what
// makes Win32 right-align the accelerator column.
std::string menuItemLabel(const MenuItem& item);
} // namespace eacp::Graphics
