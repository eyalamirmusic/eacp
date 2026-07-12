#pragma once

// The classic-COM bridges into the compositor (ICompositorInterop,
// ICompositorDesktopInterop, ICompositionDrawingSurfaceInterop). These live in
// the SDK's ABI headers, which drag in the whole ABI closure -- Windows.System,
// Windows.ApplicationModel and friends -- and cost several seconds per
// translation unit on their own. Only the handful of TUs that actually reach
// through to D2D/DXGI or create a desktop window target need them; everything
// that merely builds a visual tree should include Composition-Windows.h.

#include "Composition-Windows.h"

#include <windows.ui.composition.interop.h>
