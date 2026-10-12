#pragma once

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

namespace eacp::GPU
{
// A shader model limit, not a hardware one: each D3D12 slot costs a root DWORD.
// The codegen layer, compute and D3D12 keep to it; a hand-written render shader
// on Metal or Vulkan may go to RenderPass::maxStageTextureSlots.
constexpr int maxTextureSlots = 8;

// Bound as a UNIFORM_BUFFER_DYNAMIC, its offset supplied per draw.
constexpr int vulkanUniformBinding = 0;

constexpr int vulkanTextureBindingBase = 8;

constexpr int vulkanTextureBinding(int slot)
{
    return vulkanTextureBindingBase + slot;
}

inline int vulkanBufferBinding(int slot)
{
    return RenderPass::bufferBase + slot;
}

static_assert(vulkanTextureBinding(RenderPass::maxStageTextureSlots - 1)
                  < RenderPass::bufferBase,
              "every texture binding must stay below the storage buffers");

constexpr int vulkanComputeTextureBinding(int slot)
{
    return ComputePass::textureRegisterBase + slot;
}

constexpr int vulkanComputeUniformBinding = ComputePass::uniformBase;

// The portable slots hold the low bindings, under the textures; the slots only
// Metal and Vulkan have sit above the uniform block, so that adding them moved
// no binding an existing shader names.
constexpr int vulkanComputeBufferBinding(int slot)
{
    if (slot < ComputePass::portableBufferSlots)
        return slot;

    return vulkanComputeUniformBinding + 1 + slot - ComputePass::portableBufferSlots;
}
} // namespace eacp::GPU
