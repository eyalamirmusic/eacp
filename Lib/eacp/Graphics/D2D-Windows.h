#pragma once

// Direct2D / DirectWrite only, for the drawing primitives (Path, Font,
// TextMetrics, Image) that never touch the compositor. Anything that builds a
// visual tree wants DComp-Windows.h, which includes this.

#include <eacp/Core/Utils/WinInclude.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
