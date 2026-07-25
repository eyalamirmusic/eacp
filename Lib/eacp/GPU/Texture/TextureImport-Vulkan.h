#pragma once

#include "../Vulkan/VulkanLoader.h"

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
// Creates a VkImage over the pixel buffer's shared memory, with no copy. Both
// directions go through here and differ only in usage: SAMPLED reads a camera
// frame where it was captured, COLOR_ATTACHMENT renders a recorded frame
// straight into the buffer the encoder will read.
//
// Fills in the image, format, extent and the layout the usage implies, and
// leaves the view and memory to the caller - an imported image owns no
// VkDeviceMemory, because the platform surface is the allocation.
//
// Returns an image-less texture when the platform, the driver or the buffer
// cannot do it (not IOSurface-backed, a pixel format with no Vulkan equivalent,
// a driver without the import extension), which leaves the caller to fall back -
// to Texture::update for a read, or to the read-back capture tier for a write.
VulkanTexture importPixelBuffer(void* pixelBuffer, VkImageUsageFlags usage);
} // namespace detail
} // namespace eacp::GPU
