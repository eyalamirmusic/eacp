#include "Common.h"

#include <eacp/Graphics/Menu/MenuCommands.h>

// The Win32 menu builder and flattenCommands walk the tree independently and
// agree only on order, so every ordering test here pins that agreement.

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

// Regression: the Win32 builder asked "separator?" first and flattenCommands
// asked "submenu?" first, so ids after a both-flags item named other commands.
auto tSeparatorWinsOverSubmenu = test("MenuCommands/separatorWinsOverSubmenu") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));
    inner.add(MenuItem::withAction("Second"));

    auto malformed = MenuItem::withSubmenu("Open Recent", std::move(inner));
    malformed.isSeparator = true;

    check(classifyMenuEntry(malformed) == MenuEntryKind::Separator);

    auto file = Menu {"File"};
    file.add(std::move(malformed));
    file.add(MenuItem::withAction("Save"));

    auto bar = MenuBar {};
    bar.add(std::move(file));

    const auto commands = flattenCommands(bar);

    check(commands.size() == 1);
    check(commands[0].id == 1);
};

auto tSubmenuWinsOverResponderSelector =
    test("MenuCommands/submenuWinsOverResponderSelector") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));

    auto item = MenuItem::withSubmenu("Open Recent", std::move(inner));
    item.responderSelector = "copy:";

    check(classifyMenuEntry(item) == MenuEntryKind::Submenu);
};

// Id 0 is what WM_COMMAND and TrackPopupMenu use for "nothing was chosen".
auto tIdsStartAtOne = test("MenuCommands/idsStartAtOne") = []
{
    const auto commands = flattenCommands(twoMenuBar());

    check(commands.size() == 3);
    check(commands[0].id == 1);
    check(commands[1].id == 2);
    check(commands[2].id == 3);
};

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

// Null isChecked means "not checkable", which the backend must tell apart from
// "unchecked" to leave the item's mark alone.
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

auto tAcceleratorNamesTheModifiers =
    test("MenuCommands/acceleratorNamesTheModifiers") = []
{
    check(acceleratorText(commandKey("s")) == "Ctrl+S");
    check(acceleratorText(commandShiftKey("p")) == "Ctrl+Shift+P");
    check(acceleratorText(commandAltKey("f")) == "Ctrl+Alt+F");
};

auto tControlAndCommandCollapse = test("MenuCommands/controlAndCommandCollapse") = []
{
    auto shortcut = commandKey("s");
    shortcut.modifiers.control = true;

    check(acceleratorText(shortcut) == "Ctrl+S");
};

auto tSingleCharacterIsCapitalised =
    test("MenuCommands/singleCharacterIsCapitalised") = []
{
    check(acceleratorText(commandKey("z")) == "Ctrl+Z");
    check(acceleratorText(commandKey("/")) == "Ctrl+/");
};

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

    auto modifiersOnly = KeyEquivalent {};
    modifiersOnly.modifiers.command = true;

    check(acceleratorText(modifiersOnly).empty());
};

// The tab is what makes Win32 right-align the accelerator column.
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

    check(menuItemLabel(MenuItem::withAction("Revert File")).find('\t')
          == std::string::npos);
};

auto tSubmenuLabelIsJustTheTitle =
    test("MenuCommands/submenuLabelIsJustTheTitle") = []
{
    auto inner = Menu {"Recent"};
    inner.add(MenuItem::withAction("First"));

    check(menuItemLabel(MenuItem::withSubmenu("Open Recent", std::move(inner)))
          == "Open Recent");
};
