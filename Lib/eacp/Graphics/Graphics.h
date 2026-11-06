/** @file
    Windows, views and native 2D drawing. A `View` is a rectangle in a tree that
    paints through `paint(Context&)` and takes mouse, key and touch events; a
    `Window` is the platform window (Cocoa, Win32, UIKit, Wayland or X11) whose
    content is one view. Use views and windows from the main thread.

    To get a view on screen, pair it with its window in one member,
    `Graphics::ViewWindow<MyView>`, or skip the app struct entirely with
    `return Graphics::runWindowedApp<MyView>(options, viewArgs...);`.

    `Context`, `Font`, `TextMetrics`, `TextInput`, the retained layers and the
    image codecs exist only where `EACP_HAS_CONTEXT` is 1 (Apple and Windows).
    On Linux and Android they are left out, not stubbed: draw with the GPU and
    `eacp-text`/`eacp-ui` there. GPU rendering inside a component tree goes
    through `UI::GPUComponent`, never a native child `GPUView`, which sits
    above the whole tree unclipped and cannot present at all on Android.
*/
#pragma once

#include "Image/ImageOps.h"
#include "Helpers/DisplayLink.h"
#include "Helpers/SystemAppearance.h"
#include "HotKey/GlobalHotKey.h"
#include "Input/GameInput.h"
#include "Tray/TrayIcon.h"
#include "View/OptionalView.h"
#include "View/ViewList.h"
#include "Window/Display.h"
#include "Window/EmbeddedView.h"
#include "Window/ViewWindow.h"
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
#include "Window/KeyGrab.h"
#include "Window/NativeChildSurface.h"
#endif
