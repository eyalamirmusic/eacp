#import <AppKit/AppKit.h>
#include "AppKitMenu.h"
#include "Menu.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{
namespace
{
struct InstalledMenuBar
{
    ObjC::Ptr<NSMenu> mainMenu;
    MenuTargets targets;
};

InstalledMenuBar& installedBar()
{
    static InstalledMenuBar bar;
    return bar;
}
} // namespace

// The window is ignored here, and that is the whole difference between the two
// platforms: a macOS menu bar belongs to the application and is shown for
// whichever window is active, so there is nothing per-window to attach it to.
// Windows owns its menus per HWND and has to be told which one.
void setApplicationMenuBar(const MenuBar& bar, Window&)
{
    auto& store = installedBar();
    store.targets.clear();

    auto* mainMenu = [[NSMenu alloc] initWithTitle:@""];

    for (auto& menu: bar.menus)
    {
        auto* title = Strings::toNSString(menu.title);
        auto* container = [[NSMenuItem alloc] initWithTitle:title
                                                     action:nil
                                              keyEquivalent:@""];
        auto* nsSubmenu = buildAppKitMenu(menu, store.targets);
        nsSubmenu.title = title;
        [container setSubmenu:nsSubmenu];
        [mainMenu addItem:container];
    }

    store.mainMenu = ObjC::Ptr<NSMenu> {mainMenu};
    [NSApp setMainMenu:mainMenu];
}

Menu standardApplicationMenu(std::string applicationName)
{
    auto menu = Menu {applicationName};

    menu.add(MenuItem::withAction("About " + applicationName,
                                  [] { [NSApp orderFrontStandardAboutPanel:nil]; }));

    menu.addSeparator();

    menu.add(MenuItem::withAction("Hide " + applicationName,
                                  [] { [NSApp hide:nil]; },
                                  commandKey("h")));

    menu.add(MenuItem::withAction("Hide Others",
                                  [] { [NSApp hideOtherApplications:nil]; },
                                  commandAltKey("h")));

    menu.add(MenuItem::withAction("Show All",
                                  [] { [NSApp unhideAllApplications:nil]; }));

    menu.addSeparator();

    menu.add(MenuItem::withAction("Quit " + applicationName,
                                  [] { Apps::quit(); },
                                  commandKey("q")));

    return menu;
}

Menu standardEditMenu()
{
    auto menu = Menu {"Edit"};

    menu.add(MenuItem::withResponderSelector("Undo", "undo:", commandKey("z")));
    menu.add(
        MenuItem::withResponderSelector("Redo", "redo:", commandShiftKey("z")));

    menu.addSeparator();

    menu.add(MenuItem::withResponderSelector("Cut", "cut:", commandKey("x")));
    menu.add(MenuItem::withResponderSelector("Copy", "copy:", commandKey("c")));
    menu.add(MenuItem::withResponderSelector("Paste", "paste:", commandKey("v")));

    menu.addSeparator();

    menu.add(MenuItem::withResponderSelector("Select All",
                                             "selectAll:",
                                             commandKey("a")));

    return menu;
}

} // namespace eacp::Graphics
