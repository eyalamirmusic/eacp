#pragma once

// WinRT composition (the Visual layer) plus its classic-COM interop. This is by
// far the most expensive include in the Windows graphics stack, so only the TUs
// that actually build a visual tree should pull it in -- see D2D-Windows.h for
// the drawing-only half.

#include <eacp/Core/Utils/WinInclude.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>

#include <windows.ui.composition.interop.h>
