#pragma once

#include "VulkanContext.h"

#include "../Codegen/ShaderBindings.h"
#include "../Frame/ComputePass.h"

#include <memory>

// Internal shared types for the Linux/Vulkan GPU backend. The public GPU
// classes expose opaque void* handles (nativeBuffer/nativeLibrary/nativeState/
// ...); these structs are what those handles point to, so the separate
// translation units agree on the concrete layout without leaking Vulkan types
// into the public headers. The D3D12 sibling is Windows/D3D12Types.h. Not part
// of GPU.h.

namespace eacp::GPU
{

// How many uniform blocks one shader may bind. One, where D3D12 declares two:
// the GLSL emitter writes exactly one interface block, at
// vulkanUniformBinding for a render shader and at
// vulkanComputeUniformBinding for a kernel (Codegen/ShaderBindings.h), and a
// second would need a binding the emitter has no number for. setBytes on a
// higher slot binds nowhere and is dropped, which is what the other two
// backends do with a slot past their own ceiling.
constexpr int maxUniformSlots = 1;

constexpr int maxBufferSlots = ComputePass::maxBufferSlots;

// The compute set is laid out in the order ShaderBindings.h prints: storage
// buffers from binding 0, textures above every buffer slot, the uniform block
// on top of both. This holds the last of those to the first two.
static_assert(vulkanComputeUniformBinding
                  == ComputePass::textureRegisterBase + maxTextureSlots,
              "the compute uniform block must sit above every texture binding");

// What Buffer::nativeBuffer() points to. `use` tracks what the buffer was last
// used as within the current recording, so a barrier is only recorded when one
// recording uses the same buffer two ways: the first use in a recording is free
// because the previous recording ended with a global barrier (see
// VulkanContext::submit).
struct VulkanBufferData
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::size_t size = 0;

    BufferUse use;
    std::uint64_t recordingId = 0;

    // The persistent mapping of a BufferStorage::Streaming buffer, and the one
    // test that tells the two shapes apart: non-null means a write is a memcpy
    // here and a read comes back out of the same bytes, with nothing recorded
    // either way.
    std::byte* mapped = nullptr;
};

// Result of compiling a ShaderSource: one VkShaderModule per stage that the
// source carried, and what the modules were found to declare.
//
// Pointed to by ShaderLibrary::nativeLibrary().
struct VulkanShaderProgram
{
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModule compute = VK_NULL_HANDLE;

    // Whether any module binds something in the texture range of the descriptor
    // set. It decides whether a pipeline can be built at all, which is a
    // question only this backend has - see VulkanComputePipeline.
    bool usesTextures = false;
};

// A compiled compute pipeline and the two layouts a pass needs to bind through
// it. Pointed to by ComputePipeline::nativeState().
struct VulkanComputePipeline
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
};

// Carries the recording from CommandBuffer::beginCompute to the ComputePass.
// The CommandContext stays owned by the CommandBuffer, which submits on
// commit(); the encoder is owned by the pass.
struct VulkanComputeEncoder
{
    CommandContext* commands = nullptr;

    // Where a timed pass writes its closing timestamp. The opening one is
    // recorded by whoever began the pass; this one has to wait for the pass to
    // end, which is the pass's own business. Null and -1 when the pass carries
    // no label and is therefore not timed.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    int endQuery = -1;
};

// Closes a timed pass, wherever the pass happens to end.
inline void endTimedPass(const VulkanComputeEncoder& encoder)
{
    if (encoder.queryPool == VK_NULL_HANDLE || encoder.endQuery < 0
        || encoder.commands == nullptr)
        return;

    vkCmdWriteTimestamp2(encoder.commands->buffer,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                         encoder.queryPool,
                         static_cast<std::uint32_t>(encoder.endQuery));
}

// Records the barrier a buffer needs before being used this way, and remembers
// what it is now being used as.
//
// First use in a recording is free: every recording ends with a global barrier,
// so whatever an earlier submission wrote is already visible to whatever this
// one does first. What is left is the case that barrier cannot cover - one
// recording writing a buffer in a dispatch and reading it in the next.
inline void transitionForUse(CommandContext& commands,
                             VulkanBufferData& data,
                             const BufferUse& target)
{
    if (data.buffer == VK_NULL_HANDLE)
        return;

    if (data.recordingId != commands.recordingId)
    {
        data.recordingId = commands.recordingId;
        data.use = target;
        return;
    }

    if (data.use == target)
        return;

    VkBufferMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = data.use.stage;
    barrier.srcAccessMask = data.use.access;
    barrier.dstStageMask = target.stage;
    barrier.dstAccessMask = target.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = data.buffer;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commands.buffer, &dependency);

    data.use = target;
}

// Orders a dispatch's writes against any later read or write of the same
// memory in this recording - chained kernels, a readback copy, an indirect
// dispatch reading a grid an earlier kernel wrote. One global barrier rather
// than one per resource, which is what D3D12 records here too
// (ComputePass-Windows.cpp) and for the same reason: the pass does not know
// which of the bound buffers the kernel actually wrote.
inline void barrierAfterDispatch(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// Whether a SPIR-V module binds anything in the texture range of the descriptor
// set - bindings ComputePass::textureRegisterBase upward.
//
// The binding map gives a kernel's texture slot one binding number whether the
// slot is sampled or written (Codegen/ShaderBindings.h, matching the Metal
// indices), but Vulkan gives one binding one descriptor type: a sampled slot is
// a COMBINED_IMAGE_SAMPLER and a written one a STORAGE_IMAGE, and a shared
// layout cannot be both. Until stage 3's Texture arrives there is nothing to
// bind into either, so the layout reserves the range as sampled images and a
// module that declares anything there is refused a pipeline rather than given
// one whose descriptors it disagrees with.
//
// Reads only the decoration section, which is all that is needed to answer it -
// no type graph, and therefore nothing that a texture array or a new image kind
// could make wrong.
bool spirvBindsTextureRange(const Vector<std::uint32_t>& words);
} // namespace eacp::GPU
