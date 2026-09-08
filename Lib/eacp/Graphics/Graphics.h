#pragma once

#include "Image/ImageOps.h"
#include "Helpers/DisplayLink.h"
#include "Helpers/SystemAppearance.h"
#include "HotKey/GlobalHotKey.h"
#include "Tray/TrayIcon.h"
#include "View/ViewList.h"
#include "Window/Display.h"
#include "Window/Window.h"

// eacp-graphics defines this PUBLIC for every consumer, so a translation unit
// without it did not link the library, and an undefined macro would silently
// read as 0 and take the 2D tier away from a platform that has one.
#ifndef EACP_HAS_CONTEXT
    #error "EACP_HAS_CONTEXT is undefined - link eacp-graphics"
#endif

// The platform's own 2D drawing tier. Absent on Linux, where the headers are
// left out rather than stubbed, so a caller that needs one fails to compile
// here instead of failing to link later.
#if EACP_HAS_CONTEXT
    #include "Layers/LayerViews.h"
    #include "Primitives/TextMetrics.h"
    #include "Widgets/TextInput.h"
    #include "Window/EmbeddedView.h"
#endif
