#include "VulkanSurface.h"

namespace eacp::GPU
{
Vector<const char*> windowSystemSurfaceExtensions()
{
    return {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
}

bool createWindowSystemSurface(VkInstance instance,
                               const Graphics::NativeSurfaceHandle& handle,
                               VkSurfaceKHR& surface)
{
    if (handle.kind != Graphics::NativeSurfaceHandle::Kind::Android
        || handle.surface == nullptr || vkCreateAndroidSurfaceKHR == nullptr)
        return false;

    VkAndroidSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = static_cast<ANativeWindow*>(handle.surface);

    return vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface)
           == VK_SUCCESS;
}
} // namespace eacp::GPU
