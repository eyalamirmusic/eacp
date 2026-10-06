#pragma once

#include "ViewSurfaceBackend-Linux.h"
#include "../Window/LinuxWindowSurface-Linux.h"

struct ANativeWindow;

// The Android half of a presenting view. An activity has one surface, so the
// window's whole ANativeWindow goes to one presenting view at a time.

namespace eacp::Graphics
{
struct AndroidWindowSurface : LinuxWindowSurface
{
    ANativeWindow* nativeWindow = nullptr;

    int pixelWidth = 0;
    int pixelHeight = 0;
};

std::unique_ptr<ViewSurfaceBackend>
    makeAndroidViewSurfaceBackend(AndroidWindowSurface& window);
} // namespace eacp::Graphics
