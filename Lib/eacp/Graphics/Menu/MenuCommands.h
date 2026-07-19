#pragma once

#include "Menu.h"

namespace eacp::Graphics
{
// The portable half of a native menu bar that identifies its items by integer
// command id — which Win32 does, and macOS does not.
//
// Separated out for the same reason App-Windows-FilePicker.h factors the shell
// dialog: the Win32 calls themselves cannot be tested from here, but everything
// around them can, and that is most of the work. Ids, labels, accelerator text
// and the id-to-action lookup are all plain data, so they are compiled and
// tested on every platform rather than only on the one that consumes them.
//
// It is also not Windows-specific in principle — any backend that talks in
// command ids rather than object pointers wants exactly this.

// What a builder should do with one item, and — the point of it existing — the
// single place that decides which items consume a command id.
//
// Two independent walks assign these ids: the Win32 builder as it appends, and
// flattenCommands as it collects. They agree only if they classify every item
// identically, including the order they ask the questions in. Written out twice
// they did not: an item with isSeparator *and* a submenu went one way in one
// walk and the other way in the other, and every id after it was wrong.
//
// So neither walk decides for itself. Both switch on this.
enum class MenuEntryKind
{
    // Draws a rule, consumes no id.
    Separator,

    // Recurse into it. The header itself consumes no id.
    Submenu,

    // Consumes an id and can be chosen.
    Command,

    // Present in the model and absent from this backend — a responder-selector
    // item, which is macOS routing a command down its own chain. Consumes no
    // id, and must be skipped by both walks or their ids drift apart.
    Skipped
};

// The one definition of the precedence. Separator wins over submenu, which wins
// over everything else.
MenuEntryKind classifyMenuEntry(const MenuItem& item);

// One runnable item, flattened out of the tree.
struct MenuCommand
{
    unsigned id = 0;
    MenuAction action = [] {};
    MenuEnabled isEnabled = [] { return true; };
};

// Every actionable item in the bar, depth-first, ids assigned from `firstId`.
//
// Depth-first *in the order a builder appends them*, so a builder walking the
// same tree the same way can assign the same ids without the two agreeing on
// anything but the order. Separators and submenu headers get no id, because
// neither can be chosen; an item carrying a responder selector gets none either,
// since the platform routes those itself.
//
// Ids start at 1: zero is what TrackPopupMenu and WM_COMMAND both use to mean
// "nothing was chosen".
Vector<MenuCommand> flattenCommands(const MenuBar& bar, unsigned firstId = 1);

// Null when no command carries that id, which is the case for every WM_COMMAND
// that came from something other than this menu bar.
const MenuCommand* findCommand(const Vector<MenuCommand>& commands, unsigned id);

// "Ctrl+Shift+P". The text a Win32 menu prints after the tab in its label.
//
// Win32 menu accelerators are *decorative*: the string after '\t' is drawn and
// nothing more, and the keystroke itself has to be delivered by an accelerator
// table or by the application's own keymap. So this has to say what the app
// actually does, and cannot make it true by itself.
//
// `command` is rendered "Ctrl", which is the convention every Windows app
// follows for the primary accelerator. Note that eacp's own
// Keyboard::isCommandPressed reports the *Windows* key on Windows, so an app
// binding cmd+S today will print Ctrl+S here and respond to Win+S — see the
// note in Menu-Windows.cpp.
std::string acceleratorText(const KeyEquivalent& shortcut);

// "Save\tCtrl+S", or just "Save" when there is no shortcut. The tab is what
// makes Win32 right-align the accelerator column.
std::string menuItemLabel(const MenuItem& item);
} // namespace eacp::Graphics
