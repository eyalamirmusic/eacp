#pragma once

// WinRT composition (the Visual layer). Expensive, so only the TUs that build a
// visual tree should pull it in -- see D2D-Windows.h for the drawing-only half,
// and CompositionInterop-Windows.h for the classic-COM bridges, which cost far
// more again and are needed by only a handful of files.

#include <eacp/Core/Utils/WinInclude.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
