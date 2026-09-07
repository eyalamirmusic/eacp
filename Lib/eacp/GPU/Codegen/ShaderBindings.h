#pragma once

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

namespace eacp::GPU
{
// A shader model limit, not a hardware one: each D3D12 slot costs a root DWORD.
constexpr int maxTextureSlots = 8;

// Bound as a UNIFORM_BUFFER_DYNAMIC, its offset supplied per draw.
constexpr int vulkanUniformBinding = 0;

constexpr int vulkanTextureBinding(int slot)
{
    return maxTextureSlots + slot;
}

constexpr int vulkanBufferBinding(int slot)
{
    return RenderPass::bufferBase + slot;
}

static_assert(vulkanTextureBinding(maxTextureSlots - 1) < RenderPass::bufferBase,
              "every texture binding must stay below the storage buffers");

constexpr int vulkanComputeBufferBinding(int slot)
{
    return slot;
}

constexpr int vulkanComputeTextureBinding(int slot)
{
    return ComputePass::textureRegisterBase + slot;
}

constexpr int vulkanComputeUniformBinding = ComputePass::uniformBase;
} // namespace eacp::GPU
