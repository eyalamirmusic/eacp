#pragma once

#include "Image/ImageOps.h"
#include "Helpers/DisplayLink.h"
#include "Helpers/SystemAppearance.h"
#include "HotKey/GlobalHotKey.h"
#include "Tray/TrayIcon.h"
#include "View/ViewList.h"
#include "Window/Display.h"
#include "Window/Window.h"

// The platform's own 2D drawing tier. Absent on Linux, where the headers are
// left out rather than stubbed, so a caller that needs one fails to compile
// here instead of failing to link later.
#if EACP_HAS_CONTEXT
    #include "Layers/LayerViews.h"
    #include "Primitives/TextMetrics.h"
    #include "Widgets/TextInput.h"
    #include "Window/EmbeddedView.h"
#endif
