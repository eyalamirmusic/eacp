#pragma once

#import <AppKit/AppKit.h>
#include "Menu.h"
#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::Graphics
{
// An NSMenuItem's target is held weakly (assign), so the forwarding targets
// must outlive the menu. Build a menu into one of these and keep it alive for
// as long as the menu is in use. Each target is a runtime-built object (see
// AppKitMenu.mm) whose trigger: action forwards to a C++ MenuAction.
using MenuTargets = Vector<ObjC::Ptr<NSObject>>;

// Builds an NSMenu mirroring `menu`, appending every action-forwarding target
// it creates to `targets`.
NSMenu* buildAppKitMenu(const Menu& menu, MenuTargets& targets);

// Wraps a single MenuAction in a target so a plain NSButton (e.g. a status
// item's button) can forward its click to C++ via @selector(trigger:). The
// returned target must be kept alive for as long as the button references it.
//
// The target also answers validateMenuItem:, so a menu item built on one greys
// itself out from `isEnabled`. A null predicate means always enabled, which is
// what a plain button wants.
ObjC::Ptr<NSObject> makeActionTarget(const MenuAction& action,
                                     const MenuEnabled& isEnabled = {});
} // namespace eacp::Graphics
