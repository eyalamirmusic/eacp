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
    MenuEnabled isEnabled;
    MenuChecked isChecked;
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

// NSMenuValidation. An NSMenu autoenables its items by default, which means it
// asks each item's target this question every time the menu is about to be
// drawn — so the predicate is read from live state rather than sampled when the
// bar was built, and an app never has to rebuild a menu to grey something out.
//
// The checkmark is refreshed here too, because this is the one hook AppKit
// gives a target that fires just before every item is drawn — the same moment
// the greying question is asked. The argument is the NSMenuItem being
// validated, so a target shared with a plain NSButton (the tray icon) never
// reaches the state line: buttons carry no isChecked.
BOOL menuTargetValidate(id self, SEL, id item)
{
    auto* state = getMenuTargetState(self);

    if (state->isChecked)
        [(NSMenuItem*) item
            setState:state->isChecked() ? NSControlStateValueOn
                                        : NSControlStateValueOff];

    return state->isEnabled ? state->isEnabled() : YES;
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
        builder->addMethod(@selector(validateMenuItem:), menuTargetValidate);
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

    auto* keyEquiv = item.shortcut ? Strings::toNSString(item.shortcut->key) : @"";
    auto* nsItem = [[NSMenuItem alloc] initWithTitle:title
                                              action:@selector(trigger:)
                                       keyEquivalent:keyEquiv];

    if (item.shortcut)
        nsItem.keyEquivalentModifierMask = toModifierFlags(item.shortcut->modifiers);

    if (! item.responderSelector.empty())
    {
        // Leaving the target nil is what makes this work: AppKit then sends the
        // selector down the responder chain to the focused view, and asks that
        // same view whether to enable the item.
        nsItem.action =
            NSSelectorFromString(Strings::toNSString(item.responderSelector));
        return nsItem;
    }

    auto target = makeActionTarget(item.action, item.isEnabled, item.isChecked);
    nsItem.target = target.get();

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

ObjC::Ptr<NSObject> makeActionTarget(const MenuAction& action,
                                     const MenuEnabled& isEnabled,
                                     const MenuChecked& isChecked)
{
    auto target = ObjC::Ptr<NSObject> {[[getMenuTargetClass() alloc] init]};
    ObjC::getIvar<void*>(target.get(), "state") = new MenuTargetState();

    auto* state = getMenuTargetState(target.get());
    state->action = action;
    state->isEnabled = isEnabled;
    state->isChecked = isChecked;

    return target;
}

} // namespace eacp::Graphics
