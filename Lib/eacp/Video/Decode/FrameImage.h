#pragma once

#include "VideoFrame.h"

#include <eacp/Graphics/Image/Image.h>

namespace eacp::Video
{
// Copies into a tightly packed, straight-RGBA Image; empty for an invalid
// frame. Always costs a copy, and a buffer lock on the zero-copy path.
Graphics::Image toImage(const VideoFrame& frame);

// Locks a platform pixel buffer and copies it out as straight RGBA. Backends
// whose frames always carry CPU pixels return an empty image.
Graphics::Image nativeBufferToImage(void* buffer);
} // namespace eacp::Video
