#include "Common.h"

#include <eacp/Graphics/Menu/MenuCommands.h>

// The portable half of a command-id menu bar — which in practice means the
// Windows one, though nothing here is Windows-specific.
//
// These run on every platform on purpose. The Win32 calls themselves cannot be
// tested from a Mac, but they are a thin shell around this: the ids, the
// labels, the accelerator text and the id-to-action lookup are all plain data,
// and they are where the mistakes that matter live. Same split as
// App-Windows-FilePicker.h, and for the same reason.
//
// The load-bearing property is the one no single function owns: the builder in
// Menu-Windows.cpp walks the tree assigning ids as it appends, and
// flattenCommands walks it again assigning ids as it collects. Neither knows
// about the other; they agree only because they agree on the *order*. Every
// ordering test below is really a test of that agreement.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
MenuBar twoMenuBar()
{
    auto file = Menu {"File"};
    file.add(MenuItem::withAction("Open", [] {}, commandKey("o")));
    file.addSeparator();
    file.add(MenuItem::withAction("Save", [] {}, commandKey("s")));

    auto edit = Menu {"Edit"};
    edit.add(MenuItem::withAction("Undo", [] {}, commandKey("z")));

    auto bar = MenuBar {};
    bar.add(std::move(file));
    bar.add(std::move(edit));

    return bar;
}
} // namespace

// --- classification ---------------------------------------------------------

auto tClassifiesEachKind = test("MenuCommands/classifiesEachKind") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));

    check(classifyMenuEntry(MenuItem::separator()) == MenuEntryKind::Separator);
    check(classifyMenuEntry(MenuItem::withSubmenu("Open Recent", std::move(inner)))
          == MenuEntryKind::Submenu);
    check(classifyMenuEntry(MenuItem::withAction("Save")) == MenuEntryKind::Command);
    check(classifyMenuEntry(MenuItem::withResponderSelector("Copy", "copy:"))
          == MenuEntryKind::Skipped);
};

// An item carrying both flags has to resolve one way and only one.
//
// This is the case that made the classification worth extracting: the Win32
// builder asked "separator?" first and flattenCommands asked "submenu?" first,
// so this item consumed no ids in one walk and gave ids to a whole subtree in
// the other — and every id after it named a different command. The factories
// never build one, but MenuItem is a public struct with public fields, so it is
// constructible, and nothing about the failure would be visible until a menu
// ran the wrong thing.
auto tSeparatorWinsOverSubmenu = test("MenuCommands/separatorWinsOverSubmenu") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));
    inner.add(MenuItem::withAction("Second"));

    auto malformed = MenuItem::withSubmenu("Open Recent", std::move(inner));
    malformed.isSeparator = true;

    check(classifyMenuEntry(malformed) == MenuEntryKind::Separator);

    // And the walk agrees: no ids come out of the subtree it does not recurse
    // into.
    auto file = Menu {"File"};
    file.add(std::move(malformed));
    file.add(MenuItem::withAction("Save"));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    const auto commands = flattenCommands(bar);

    check(commands.size() == 1);
    check(commands[0].id == 1);
};

// A responder-selector item that also carries a submenu is a submenu: the
// children are real and the selector is meaningless on a header.
auto tSubmenuWinsOverResponderSelector =
    test("MenuCommands/submenuWinsOverResponderSelector") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));

    auto item = MenuItem::withSubmenu("Open Recent", std::move(inner));
    item.responderSelector = "copy:";

    check(classifyMenuEntry(item) == MenuEntryKind::Submenu);
};

// --- ids --------------------------------------------------------------------

// Ids start at 1. Zero is what WM_COMMAND and TrackPopupMenu both use for
// "nothing was chosen", so an item carrying it could never be told apart from
// a dismissed menu.
auto tIdsStartAtOne = test("MenuCommands/idsStartAtOne") = []
{
    const auto commands = flattenCommands(twoMenuBar());

    check(commands.size() == 3);
    check(commands[0].id == 1);
    check(commands[1].id == 2);
    check(commands[2].id == 3);
};

// Separators take no id, because nothing can choose one — and if they did, the
// ids would drift out of step with the builder, which appends separators
// without consuming one.
auto tSeparatorsTakeNoId = test("MenuCommands/separatorsTakeNoId") = []
{
    auto menu = Menu {"File"};
    menu.addSeparator();
    menu.add(MenuItem::withAction("Open"));
    menu.addSeparator();
    menu.addSeparator();
    menu.add(MenuItem::withAction("Save"));

    auto bar = MenuBar {};
    bar.add(std::move(menu));

    const auto commands = flattenCommands(bar);

    check(commands.size() == 2);
    check(commands[0].id == 1);
    check(commands[1].id == 2);
};

// A submenu header is not itself runnable, so it takes no id either — its
// children get theirs, in the order the builder recurses into them.
auto tSubmenusAreWalkedInPlace = test("MenuCommands/submenusAreWalkedInPlace") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));
    inner.add(MenuItem::withAction("Second"));

    auto file = Menu {"File"};
    file.add(MenuItem::withAction("Open"));
    file.add(MenuItem::withSubmenu("Open Recent", std::move(inner)));
    file.add(MenuItem::withAction("Save"));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    auto ran = std::string {};

    // Rebuild with actions so the order can be checked by what runs.
    auto innerB = Menu {"Recent"};
    innerB.add(MenuItem::withAction("First", [&ran] { ran = "first"; }));
    innerB.add(MenuItem::withAction("Second", [&ran] { ran = "second"; }));

    auto fileB = Menu {"File"};
    fileB.add(MenuItem::withAction("Open", [&ran] { ran = "open"; }));
    fileB.add(MenuItem::withSubmenu("Open Recent", std::move(innerB)));
    fileB.add(MenuItem::withAction("Save", [&ran] { ran = "save"; }));

    auto barB = MenuBar {};
    barB.add(std::move(fileB));

    const auto commands = flattenCommands(barB);

    // Open, then the submenu's two, then Save — depth-first, in place.
    check(commands.size() == 4);

    commands[0].action();
    check(ran == "open");

    commands[1].action();
    check(ran == "first");

    commands[2].action();
    check(ran == "second");

    commands[3].action();
    check(ran == "save");
};

// A responder-selector item is macOS routing its own command. Windows skips it
// when building, so it must be skipped when flattening too — otherwise every id
// after it is off by one and the menu runs the wrong commands.
auto tResponderItemsTakeNoId = test("MenuCommands/responderItemsTakeNoId") = []
{
    auto ran = std::string {};

    auto edit = Menu {"Edit"};
    edit.add(MenuItem::withResponderSelector("Copy", "copy:", commandKey("c")));
    edit.add(MenuItem::withAction("Find", [&ran] { ran = "find"; }));

    auto bar = MenuBar {};
    bar.add(std::move(edit));

    const auto commands = flattenCommands(bar);

    check(commands.size() == 1);
    check(commands[0].id == 1);

    commands[0].action();
    check(ran == "find");
};

auto tIdsCarryTheirAction = test("MenuCommands/idsCarryTheirAction") = []
{
    auto ran = std::string {};

    auto file = Menu {"File"};
    file.add(MenuItem::withAction("Open", [&ran] { ran = "open"; }));
    file.add(MenuItem::withAction("Save", [&ran] { ran = "save"; }));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    const auto commands = flattenCommands(bar);

    findCommand(commands, 2)->action();

    check(ran == "save");
};

auto tUnknownIdFindsNothing = test("MenuCommands/unknownIdFindsNothing") = []
{
    const auto commands = flattenCommands(twoMenuBar());

    check(findCommand(commands, 0) == nullptr);
    check(findCommand(commands, 99) == nullptr);
    check(findCommand(commands, 1) != nullptr);
};

// The predicate travels with the command, so WM_INITMENUPOPUP can ask it just
// before the popup is drawn rather than the app rebuilding its bar.
auto tPredicateTravelsWithTheCommand =
    test("MenuCommands/predicateTravelsWithTheCommand") = []
{
    auto available = false;

    auto file = Menu {"File"};
    file.add(MenuItem::withAction(
        "Revert", [] {}, {}, [&available] { return available; }));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    const auto commands = flattenCommands(bar);

    check(!commands[0].isEnabled());

    available = true;
    check(commands[0].isEnabled());
};

// The checked predicate travels the same way, so WM_INITMENUPOPUP can refresh
// the mark live. And a command that never mentioned checking must arrive with
// a null predicate — null is "not checkable", which the backend must be able
// to distinguish from "unchecked" to leave the item's mark alone.
auto tCheckedPredicateTravelsWithTheCommand =
    test("MenuCommands/checkedPredicateTravelsWithTheCommand") = []
{
    auto selected = false;

    auto view = Menu {"View"};
    view.add(MenuItem::withCheckableAction(
        "FaceTime HD Camera", [] {}, [&selected] { return selected; }));
    view.add(MenuItem::withAction("Refresh"));

    auto bar = MenuBar {};
    bar.add(std::move(view));

    const auto commands = flattenCommands(bar);

    check(commands[0].isChecked != nullptr);
    check(!commands[0].isChecked());

    selected = true;
    check(commands[0].isChecked());

    check(commands[1].isChecked == nullptr);
};

auto tEmptyBarHasNoCommands = test("MenuCommands/emptyBarHasNoCommands") = []
{ check(flattenCommands(MenuBar {}).size() == 0); };

// --- accelerator text -------------------------------------------------------

auto tAcceleratorNamesTheModifiers =
    test("MenuCommands/acceleratorNamesTheModifiers") = []
{
    check(acceleratorText(commandKey("s")) == "Ctrl+S");
    check(acceleratorText(commandShiftKey("p")) == "Ctrl+Shift+P");
    check(acceleratorText(commandAltKey("f")) == "Ctrl+Alt+F");
};

// Both spellings of the primary modifier collapse to one "Ctrl+". A chord with
// control and command set would otherwise read "Ctrl+Ctrl+S".
auto tControlAndCommandCollapse = test("MenuCommands/controlAndCommandCollapse") = []
{
    auto shortcut = commandKey("s");
    shortcut.modifiers.control = true;

    check(acceleratorText(shortcut) == "Ctrl+S");
};

// A single character is capitalised — "Ctrl+S", not "Ctrl+s".
auto tSingleCharacterIsCapitalised =
    test("MenuCommands/singleCharacterIsCapitalised") = []
{
    check(acceleratorText(commandKey("z")) == "Ctrl+Z");

    // Punctuation has no case and is passed straight through.
    check(acceleratorText(commandKey("/")) == "Ctrl+/");
};

// A named key already names itself, so it is capitalised at the front rather
// than uppercased whole — "Escape", not "ESCAPE".
auto tNamedKeysKeepTheirCase = test("MenuCommands/namedKeysKeepTheirCase") = []
{
    auto shortcut = KeyEquivalent {};
    shortcut.key = "escape";

    check(acceleratorText(shortcut) == "Escape");

    shortcut.key = "pageup";
    check(acceleratorText(shortcut) == "Pageup");
};

auto tEmptyKeyHasNoAccelerator = test("MenuCommands/emptyKeyHasNoAccelerator") = []
{
    check(acceleratorText(KeyEquivalent {}).empty());

    // Modifiers alone are not a shortcut, so this must not come back "Ctrl+".
    auto modifiersOnly = KeyEquivalent {};
    modifiersOnly.modifiers.command = true;

    check(acceleratorText(modifiersOnly).empty());
};

// --- labels -----------------------------------------------------------------

// The tab is what makes Win32 right-align the accelerator column. Without it
// the shortcut runs on after the title as ordinary text.
auto tLabelJoinsWithATab = test("MenuCommands/labelJoinsWithATab") = []
{
    check(menuItemLabel(MenuItem::withAction(
              "Save", [] {}, commandKey("s")))
          == "Save\tCtrl+S");
};

auto tLabelWithoutShortcutIsBare =
    test("MenuCommands/labelWithoutShortcutIsBare") = []
{
    check(menuItemLabel(MenuItem::withAction("Revert File")) == "Revert File");

    // And no trailing tab, which would open an empty accelerator column.
    check(menuItemLabel(MenuItem::withAction("Revert File")).find('\t')
          == std::string::npos);
};

// A submenu header is a label with no accelerator of its own.
auto tSubmenuLabelIsJustTheTitle =
    test("MenuCommands/submenuLabelIsJustTheTitle") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));

    check(menuItemLabel(MenuItem::withSubmenu("Open Recent", std::move(inner)))
          == "Open Recent");
};
