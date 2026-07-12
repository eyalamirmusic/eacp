#import <AppKit/AppKit.h>
#include "AppKitMenu.h"
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{
namespace
{
// Objective-C target that forwards an NSMenuItem (or NSButton) action to a
// C++ MenuAction. Shared by the application menu bar and the tray icon.
// Runtime classes get no automatic C++ ivar construction, so the action
// lives behind one raw "state" pointer, created with the target and deleted
// in its dealloc.
struct MenuTargetState
{
    MenuAction action;
};

MenuTargetState* getMenuTargetState(id self)
{
    return (MenuTargetState*) ObjC::getIvar<void*>(self, "state");
}

void menuTargetTrigger(id self, SEL, id)
{
    auto& action = getMenuTargetState(self)->action;

    if (action)
        action();
}

void menuTargetDealloc(id self, SEL)
{
    delete getMenuTargetState(self);
    ObjC::sendSuper<void>(self, [NSObject class], @selector(dealloc));
}

Class getMenuTargetClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpMenuTarget");

        builder->addIvar<void*>("state");
        builder->addMethod(@selector(trigger:), menuTargetTrigger);
        builder->addMethod(@selector(dealloc), menuTargetDealloc);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
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

ObjC::Ptr<NSObject> makeActionTarget(const MenuAction& action)
{
    auto target = ObjC::Ptr<NSObject> {[[getMenuTargetClass() alloc] init]};
    ObjC::getIvar<void*>(target.get(), "state") = new MenuTargetState();
    getMenuTargetState(target.get())->action = action;
    return target;
}

} // namespace eacp::Graphics
