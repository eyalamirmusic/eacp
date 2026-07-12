#pragma once

// Direct2D / DirectWrite only. Kept separate from Composition-Windows.h because
// the WinRT composition headers cost ~2s of parse time per translation unit and
// the drawing primitives (Path, Font, TextMetrics, Image) never touch WinRT.
// Include the narrowest of the two that a TU actually needs.

#include <eacp/Core/Utils/WinInclude.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
