#pragma once

#include <eacp/Core/Utils/Containers.h>

#include "../Texture/Texture.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

// Process-wide Vulkan plumbing shared by every Vulkan translation unit: the
// instance, physical and logical device, the graphics queue, the timeline
// semaphore that orders CPU/GPU work, a pool of command buffers recycled once
// their timeline value passes, and the descriptor plumbing textures bind
// through. The direct counterpart of D3D12Context, and deliberately shaped like
// it -- submit() returning a monotonic value that hasCompleted()/waitFor() are
// asked about is exactly a timeline semaphore, so the two backends can share
// their lifetime reasoning. Not part of GPU.h.

namespace eacp::GPU
{
// One recording in flight: a command buffer plus the transient resources (per-
// draw staging copies) the recorded commands reference. Everything is released
// together once timelineValue has passed.
struct CommandContext
{
    VkCommandBuffer list = VK_NULL_HANDLE;
    std::uint64_t timelineValue = 0;

    // Identifies the recording so a resource can tell whether it has already
    // been transitioned within this one.
    std::uint64_t recordingId = 0;

    Vector<VkBuffer> transientBuffers;
    Vector<VkDeviceMemory> transientMemory;
    Vector<VkDescriptorSet> descriptorSets;
};

// A device allocation paired with the object it backs. Vulkan has no committed-
// resource shorthand, so every buffer and image carries its own memory here;
// suballocation is a later concern, not a prototype one.
struct Allocation
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
};

// How many texture slots a shader may declare. Each one occupies
// samplingConfigurations consecutive bindings, so that the sampling a shader
// declared picks a binding rather than being bound per draw -- the arrangement
// D3D12 reaches with static samplers in its root signature. See TextureSampling.
constexpr int maxTextureSlots = 4;

// How many storage buffers one kernel may bind. Inputs and outputs share the
// slot space (Metal binds both as a device buffer), so this is the total.
constexpr int maxStorageBuffers = 8;

class VulkanContext
{
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool isValid() const { return device != VK_NULL_HANDLE; }

    VkInstance getInstance() const { return instance; }
    VkPhysicalDevice getPhysicalDevice() const { return physicalDevice; }
    VkDevice getDevice() const { return device; }
    VkQueue getQueue() const { return queue; }
    std::uint32_t getQueueFamily() const { return queueFamily; }

    // The largest uniform block a draw may bind. Uniforms travel as push
    // constants, which is the natural fit for eacp's setVertexBytes/
    // setFragmentBytes (small, per-draw, bound to both stages at once) but is
    // capped by the device: the Vulkan floor is 128 bytes, which holds one
    // float4x4 plus change. MoltenVK reports 4096. A block over the limit is
    // refused by the render pass rather than silently truncated.
    std::uint32_t maxUniformBytes() const { return pushConstantLimit; }

    // An open command buffer ready for recording. Owned by the caller until it
    // is handed back through submit() or discard().
    CommandContext* acquire();

    // Ends and submits the recording, signals the timeline and recycles the
    // context. Returns the value that completes when the GPU finishes.
    std::uint64_t submit(CommandContext* commands);

    // Presenting sibling: waits on the swapchain's acquire semaphore before the
    // colour attachment stage, and signals one the present can wait on.
    std::uint64_t submit(CommandContext* commands,
                         VkSemaphore waitFirst,
                         VkSemaphore signalWhenDone);

    // A binary semaphore for swapchain acquire/present handoff. Owned by the
    // caller, which is the view that built the swapchain.
    VkSemaphore makeSemaphore();

    // Whether the device offers VK_KHR_swapchain. False on a headless driver,
    // where a GPUView renders off-screen and never presents.
    bool canPresent() const { return presentationSupported; }

    // Whether a VkImage can be backed by a surface the platform already shares
    // -- an IOSurface behind a CVPixelBuffer. False leaves wrapPixelBuffer
    // returning an invalid texture, so callers fall back to Texture::update.
    bool canImportSurfaces() const { return surfaceImportSupported; }

    // Recycles a recording that should never reach the GPU.
    void discard(CommandContext* commands);

    std::uint64_t lastSubmitted() const { return lastSubmittedValue; }
    bool hasCompleted(std::uint64_t value) const;
    void waitFor(std::uint64_t value);
    void waitIdle();

    // Picks a memory type satisfying typeBits and properties, or -1.
    std::uint32_t findMemoryType(std::uint32_t typeBits,
                                 VkMemoryPropertyFlags properties) const;

    // Allocates and binds device memory for an already-created buffer or image.
    Allocation allocateFor(VkBuffer buffer, VkMemoryPropertyFlags properties);
    Allocation allocateFor(VkImage image, VkMemoryPropertyFlags properties);

    // A host-visible staging buffer pre-filled with the bytes, parked on the
    // recording so it outlives GPU execution.
    VkBuffer makeStagingBuffer(CommandContext& commands,
                               const void* data,
                               std::size_t bytes);

    // Keeps an object alive until the GPU has finished everything submitted so
    // far. Buffer, texture and pipeline destructors route through here rather
    // than destroying something a still-open recording references.
    void deferDestroy(VkBuffer buffer, VkDeviceMemory memory);
    void deferDestroy(VkImage image, VkImageView view, VkDeviceMemory memory);
    void deferDestroy(VkPipeline pipeline);

    // Dynamic rendering, loaded from the KHR extension because the device here
    // reports Vulkan 1.2. Core entry points in 1.3.
    void beginRendering(VkCommandBuffer list, const VkRenderingInfoKHR& info) const;
    void endRendering(VkCommandBuffer list) const;

    // One layout shared by every render pipeline, so a descriptor set stays
    // bound across a pipeline change instead of being invalidated by it.
    VkPipelineLayout getRenderPipelineLayout() const { return renderPipelineLayout; }

    // The compute sibling: one set of storage buffers plus the same push-constant
    // range, shared by every compute pipeline for the same reason.
    VkPipelineLayout getComputePipelineLayout() const
    {
        return computePipelineLayout;
    }

    // A texture descriptor set for one draw, freed with the recording it was
    // allocated against. Returns null when the pool is exhausted.
    VkDescriptorSet acquireTextureSet(CommandContext& commands);

    // The storage-buffer sibling, for one dispatch.
    VkDescriptorSet acquireStorageSet(CommandContext& commands);

private:
    bool createInstance();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createCommandPool();
    bool createTimeline();
    bool createSamplers();
    bool createDescriptorLayout();
    VkDescriptorSet acquireSet(CommandContext& commands,
                               VkDescriptorSetLayout layout);
    void purgeRetired();
    void destroyAll();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    std::uint32_t pushConstantLimit = 128;
    bool presentationSupported = false;
    bool surfaceImportSupported = false;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;

    // One immutable sampler per sampling configuration, baked into the set
    // layout so a binding carries its sampler and a draw never picks one.
    VkSampler samplers[samplingConfigurations] = {};
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout storageSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout renderPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::uint64_t nextTimelineValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t recordingCounter = 0;

    PFN_vkCmdBeginRenderingKHR cmdBeginRendering = nullptr;
    PFN_vkCmdEndRenderingKHR cmdEndRendering = nullptr;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;

    struct Retired
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::uint64_t timelineValue = 0;
    };

    Vector<Retired> retired;
};

// The process-wide context, created on first use. Main-thread only, like the
// rest of the GPU backend.
VulkanContext& getVulkanContext();
} // namespace eacp::GPU
