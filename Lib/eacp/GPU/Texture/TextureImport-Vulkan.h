#pragma once

#include <vulkan/vulkan.h>

// Internal seam between the portable Vulkan Texture (Texture-Vulkan.cpp) and the
// per-platform piece that knows what a "platform pixel buffer" is and how the
// platform shares its pixels with the GPU: an IOSurface behind a CVPixelBuffer
// on Apple, a dma-buf on Linux. Vulkan can import either, but neither is
// spellable portably, so the knowledge is confined here the way presentation is
// confined to GPUViewSurface-Vulkan.h. Not part of GPU.h.

namespace eacp::GPU
{
struct VulkanTexture;

namespace detail
{
// Creates a VkImage that samples the pixel buffer's shared memory directly, with
// no copy: the camera and video path. Fills in the image, format and extent and
// leaves the view, layout and memory to the caller - an imported image owns no
// VkDeviceMemory, because the platform surface is the allocation.
//
// Returns an image-less texture when the platform, the driver or the buffer
// cannot do it (not IOSurface-backed, a pixel format with no Vulkan equivalent,
// a driver without the import extension), which leaves the caller holding an
// invalid Texture - the same answer D3D12 gives.
VulkanTexture importPixelBuffer(void* pixelBuffer);
} // namespace detail
} // namespace eacp::GPU
