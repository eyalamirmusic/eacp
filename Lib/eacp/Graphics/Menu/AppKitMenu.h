#pragma once

#import <AppKit/AppKit.h>
#include "Menu.h"
#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::Graphics
{
// An NSMenuItem's target is held weakly, so keep this alive for as long as the
// menu is in use.
using MenuTargets = Vector<ObjC::Ptr<NSObject>>;

// Appends every action-forwarding target it creates to `targets`.
NSMenu* buildAppKitMenu(const Menu& menu, MenuTargets& targets);

// Lets a plain NSButton forward its click to C++ via @selector(trigger:); must
// outlive the button. Also answers validateMenuItem:, where a null isEnabled
// means always enabled and a null isChecked means no checkmark management.
ObjC::Ptr<NSObject> makeActionTarget(const MenuAction& action,
                                     const MenuEnabled& isEnabled = {},
                                     const MenuChecked& isChecked = {});
} // namespace eacp::Graphics
