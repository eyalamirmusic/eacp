#pragma once

// Not the Graphics.h umbrella: eacp-gpu-codegen builds without eacp-graphics.
#include <eacp/Graphics/Primitives/Primitives.h>

#include <array>
#include <concepts>

namespace eacp::GPU
{
// The most colour textures one pass renders into, and one pipeline writes: what
// Metal and D3D12 offer and what bgfx asks for. A Vulkan device may offer fewer,
// down to the spec's floor of four, and refuses a pass or pipeline beyond it.
constexpr int maxColorAttachments = 8;
} // namespace eacp::GPU
