#pragma once

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

// Where a shader resource lands, for the backends whose answer is a number the
// emitter has to print. RenderPass and ComputePass already hold the Metal
// indices and the D3D registers; this holds the two things they do not - the
// slot ceiling both root signatures and the Vulkan descriptor set are built
// against, and the Vulkan binding map - so no backend has to restate one.

namespace eacp::GPU
{
// Eight, and the number is a shader model limit rather than a hardware one: on
// D3D12 the slots are single-descriptor tables, which cost one root DWORD each,
// so this is nearly free. It was four until a port needed five in one program -
// Doom 3 lights a surface from a bump map, a falloff, a light projection, a
// diffuse map and a specular map, none of which fold into another.
//
// It lives here rather than beside the D3D12 root signature because the shader
// emitter needs it too: the Vulkan binding map reserves exactly this many
// bindings for textures, and the range above them is where the storage buffers
// start.
constexpr int maxTextureSlots = 8;

// The Vulkan binding map. One descriptor set holds all three kinds at once -
// unlike Metal, which gives textures an index space of their own, and unlike
// D3D12, which splits them across b/t/u - so each kind gets a range wide enough
// that no layout can push one into the next.
//
// The uniform block takes binding 0 (bound as a UNIFORM_BUFFER_DYNAMIC, its
// offset supplied per draw); textures take the maxTextureSlots bindings above
// it; storage buffers start at RenderPass::bufferBase, the very index the Metal
// binder writes, which leaves every texture slot below them.
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

// A kernel's bindings are the Metal indices verbatim: buffers from zero,
// textures above every buffer slot, the uniform block on top of both. The two
// bases are ComputePass's own, so a kernel's descriptor set and its Metal
// argument table cannot drift apart.
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
