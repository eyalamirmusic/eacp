#pragma once

#include "VulkanContext.h"

#include <eacp/Graphics/View/View-Linux.h>

// The window-system half of presenting, one file per platform: each build
// defines only its own VK_USE_PLATFORM_* macros, so only its own create-info
// structs exist.

namespace eacp::GPU
{
Vector<const char*> windowSystemSurfaceExtensions();

bool createWindowSystemSurface(VkInstance instance,
                               const Graphics::NativeSurfaceHandle& handle,
                               VkSurfaceKHR& surface);
} // namespace eacp::GPU
