#include "TextureImport-Vulkan.h"

#include "../Vulkan/VulkanTypes.h"

// The Windows half of zero-copy pixel-buffer import, unwired -- as it is on the
// D3D12 backend, whose GPUView::renderNativeContentToTarget returns false for the
// same reason. There is no CVPixelBuffer here: the equivalent would import a DXGI
// shared handle through VK_KHR_external_memory_win32, and nothing asks for one
// yet. The zero-copy capture tier's only caller is Encoder-Apple.mm, and
// Camera-Windows delivers frames as pixels, which is Texture::update's path.
//
// An image-less texture is the documented answer for "the platform cannot do
// it", so every caller already falls back.

namespace eacp::GPU::detail
{
VulkanTexture importPixelBuffer(void*, VkImageUsageFlags)
{
    return {};
}
} // namespace eacp::GPU::detail
