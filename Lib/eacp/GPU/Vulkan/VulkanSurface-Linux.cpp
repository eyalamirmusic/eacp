#include "VulkanSurface.h"

namespace eacp::GPU
{
namespace
{
// The instance enables a platform extension only where the driver offered
// it, so volk leaves the entry point of the other one null.
bool createWaylandSurface(VkInstance instance,
                          const Graphics::NativeSurfaceHandle& handle,
                          VkSurfaceKHR& surface)
{
    if (vkCreateWaylandSurfaceKHR == nullptr)
        return false;

    VkWaylandSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = static_cast<wl_display*>(handle.connection);
    info.surface = static_cast<wl_surface*>(handle.surface);

    return vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface)
           == VK_SUCCESS;
}

bool createXcbSurface(VkInstance instance,
                      const Graphics::NativeSurfaceHandle& handle,
                      VkSurfaceKHR& surface)
{
    if (vkCreateXcbSurfaceKHR == nullptr)
        return false;

    VkXcbSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    info.connection = static_cast<xcb_connection_t*>(handle.connection);
    info.window = static_cast<xcb_window_t>(handle.window);

    return vkCreateXcbSurfaceKHR(instance, &info, nullptr, &surface) == VK_SUCCESS;
}
} // namespace

Vector<const char*> windowSystemSurfaceExtensions()
{
    return {VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
            VK_KHR_XCB_SURFACE_EXTENSION_NAME};
}

// One branch per window system, and nothing past the caller cares which.
bool createWindowSystemSurface(VkInstance instance,
                               const Graphics::NativeSurfaceHandle& handle,
                               VkSurfaceKHR& surface)
{
    using Kind = Graphics::NativeSurfaceHandle::Kind;

    switch (handle.kind)
    {
        case Kind::Wayland:
            return createWaylandSurface(instance, handle, surface);

        case Kind::X11:
            return createXcbSurface(instance, handle, surface);

        case Kind::Android:
        case Kind::None:
            return false;
    }

    return false;
}
} // namespace eacp::GPU
