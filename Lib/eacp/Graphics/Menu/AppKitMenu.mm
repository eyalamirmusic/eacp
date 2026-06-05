#import <AppKit/AppKit.h>
#include "AppKitMenu.h"
#include <eacp/Core/ObjC/Strings.h>

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

NSMenuItem* buildAppKitMenuItem(const MenuItem& item, MenuTargets& targets)
{
    if (item.isSeparator)
        return [NSMenuItem separatorItem];

    auto* title = Strings::toNSString(item.title);

    if (item.submenu)
    {
        auto* submenuItem = [[NSMenuItem alloc] initWithTitle:title
                                                       action:nil
                                                keyEquivalent:@""];
        auto* nsSubmenu = buildAppKitMenu(*item.submenu, targets);
        nsSubmenu.title = title;
        [submenuItem setSubmenu:nsSubmenu];
        return submenuItem;
    }

    auto target = makeActionTarget(item.action);

    auto* keyEquiv = item.shortcut ? Strings::toNSString(item.shortcut->key) : @"";
    auto* nsItem = [[NSMenuItem alloc] initWithTitle:title
                                              action:@selector(trigger:)
                                       keyEquivalent:keyEquiv];
    nsItem.target = target.get();

    if (item.shortcut)
        nsItem.keyEquivalentModifierMask = toModifierFlags(item.shortcut->modifiers);

    targets.add(std::move(target));
    return nsItem;
}
} // namespace

NSMenu* buildAppKitMenu(const Menu& menu, MenuTargets& targets)
{
    auto* nsMenu = [[NSMenu alloc] initWithTitle:Strings::toNSString(menu.title)];

    for (auto& item: menu.items)
        [nsMenu addItem:buildAppKitMenuItem(item, targets)];

    return nsMenu;
}

ObjC::Ptr<EacpMenuTarget> makeActionTarget(const MenuAction& action)
{
    auto target = ObjC::Ptr<EacpMenuTarget> {[[EacpMenuTarget alloc] init]};
    target.get()->action = action;
    return target;
}

} // namespace eacp::Graphics
