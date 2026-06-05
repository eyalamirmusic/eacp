#pragma once

#import <AppKit/AppKit.h>
#include "Menu.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <ea_data_structures/Structures/Vector.h>

// Objective-C target that forwards an NSMenuItem (or NSButton) action to a
// C++ MenuAction. Shared by the application menu bar and the tray icon.
@interface EacpMenuTarget : NSObject
{
@public
    eacp::Graphics::MenuAction action;
}
- (void)trigger:(id)sender;
@end

namespace eacp::Graphics
{
// An NSMenuItem's target is held weakly (assign), so the forwarding targets
// must outlive the menu. Build a menu into one of these and keep it alive for
// as long as the menu is in use.
using MenuTargets = EA::Vector<ObjC::Ptr<EacpMenuTarget>>;

// Builds an NSMenu mirroring `menu`, appending every action-forwarding target
// it creates to `targets`.
NSMenu* buildAppKitMenu(const Menu& menu, MenuTargets& targets);

// Wraps a single MenuAction in a target so a plain NSButton (e.g. a status
// item's button) can forward its click to C++. The returned target must be
// kept alive for as long as the button references it.
ObjC::Ptr<EacpMenuTarget> makeActionTarget(const MenuAction& action);
} // namespace eacp::Graphics
