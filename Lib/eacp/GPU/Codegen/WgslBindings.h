#pragma once

#include "ShaderBindings.h"

namespace eacp::GPU
{
// Where the WGSL emitter puts each resource, all in @group(0). The WebGPU
// backend reflects its bind group layout from these declarations, so these
// numbers are the whole contract between the two. A sampler is one per sampled
// texture slot, as on Metal, and sits at the same place in both pipeline kinds.
constexpr int wgslGroup = 0;
constexpr int wgslSamplerBase = 32;

constexpr int wgslSamplerBinding(int slot)
{
    return wgslSamplerBase + slot;
}

// Bound with a dynamic offset, like the Vulkan block.
constexpr int wgslUniformBinding = vulkanUniformBinding;

constexpr int wgslTextureBinding(int slot)
{
    return vulkanTextureBinding(slot);
}

constexpr int wgslBufferBinding(int slot)
{
    return vulkanBufferBinding(slot);
}

constexpr int wgslComputeBufferBinding(int slot)
{
    return vulkanComputeBufferBinding(slot);
}

constexpr int wgslComputeTextureBinding(int slot)
{
    return vulkanComputeTextureBinding(slot);
}

constexpr int wgslComputeUniformBinding = vulkanComputeUniformBinding;

static_assert(wgslBufferBinding(0) >= wgslTextureBinding(maxTextureSlots),
              "render storage buffers must stay above every texture binding");
static_assert(wgslComputeUniformBinding < wgslSamplerBase,
              "the samplers must stay above every compute binding");
static_assert(wgslComputeTextureBinding(maxTextureSlots - 1)
                  < wgslComputeUniformBinding,
              "compute textures must stay below the uniform block");
} // namespace eacp::GPU
