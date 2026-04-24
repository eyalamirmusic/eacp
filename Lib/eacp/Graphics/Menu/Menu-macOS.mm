#import <AppKit/AppKit.h>
#include "Menu.h"
#include <eacp/Core/App/App.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/Strings.h>
#include <vector>

@interface EacpMenuTarget : NSObject
{
@public
    eacp::Graphics::MenuAction action;
}
- (void)trigger:(id)sender;
@end

@implementation EacpMenuTarget
- (void)trigger:(id)sender
{
    if (action)
        action();
}
@end

namespace eacp::Graphics
{
namespace
{
struct InstalledMenuBar
{
    ObjC::Ptr<NSMenu> mainMenu;
    std::vector<ObjC::Ptr<EacpMenuTarget>> targets;
};

InstalledMenuBar& installedBar()
{
    static InstalledMenuBar bar;
    return bar;
}

NSEventModifierFlags toModifierFlags(const ModifierKeys& mods)
{
    auto flags = NSEventModifierFlags(0);

    if (mods.command)
        flags |= NSEventModifierFlagCommand;
    if (mods.shift)
        flags |= NSEventModifierFlagShift;
    if (mods.alt)
        flags |= NSEventModifierFlagOption;
    if (mods.control)
        flags |= NSEventModifierFlagControl;

    return flags;
}

NSMenu* buildNSMenu(const Menu& menu, InstalledMenuBar& store);

NSMenuItem* buildNSMenuItem(const MenuItem& item, InstalledMenuBar& store)
{
    if (item.isSeparator)
        return [NSMenuItem separatorItem];

    auto* title = Strings::toNSString(item.title);

    if (item.submenu)
    {
        auto* submenuItem = [[NSMenuItem alloc] initWithTitle:title
                                                       action:nil
                                                keyEquivalent:@""];
        auto* nsSubmenu = buildNSMenu(*item.submenu, store);
        nsSubmenu.title = title;
        [submenuItem setSubmenu:nsSubmenu];
        return submenuItem;
    }

    auto target = ObjC::Ptr<EacpMenuTarget> {[[EacpMenuTarget alloc] init]};
    target.get()->action = item.action;

    auto* keyEquiv = item.shortcut ? Strings::toNSString(item.shortcut->key) : @"";
    auto* nsItem = [[NSMenuItem alloc] initWithTitle:title
                                              action:@selector(trigger:)
                                       keyEquivalent:keyEquiv];
    nsItem.target = target.get();

    if (item.shortcut)
        nsItem.keyEquivalentModifierMask = toModifierFlags(item.shortcut->modifiers);

    store.targets.push_back(std::move(target));
    return nsItem;
}

NSMenu* buildNSMenu(const Menu& menu, InstalledMenuBar& store)
{
    auto* nsMenu = [[NSMenu alloc] initWithTitle:Strings::toNSString(menu.title)];

    for (auto& item: menu.items)
        [nsMenu addItem:buildNSMenuItem(item, store)];

    return nsMenu;
}
} // namespace

void setApplicationMenuBar(MenuBar bar)
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
        auto* nsSubmenu = buildNSMenu(menu, store);
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

} // namespace eacp::Graphics
