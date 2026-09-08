#pragma once

#include "../Texture/Texture.h"

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/Containers.h>

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

// VulkanShared is one per process, VulkanContext one per GPU::Device. Nothing
// in a context is synchronized. Not part of GPU.h.

namespace eacp::GPU
{
class Device;
class VulkanContext;
struct VulkanTextureData;

// A range of a recording's upload arena, valid until that recording completes.
struct UploadRange
{
    bool isValid() const { return mapped != nullptr; }

    VkBuffer buffer = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
    VkDeviceSize offset = 0;
};

// A block of the constant ring, as a UNIFORM_BUFFER_DYNAMIC binds one.
struct ConstantRange
{
    bool isValid() const { return buffer != VK_NULL_HANDLE; }

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;

    // The block rounded up to std140's multiple of 16, not its byte count.
    VkDeviceSize range = 0;
};

struct BufferUse
{
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;

    bool operator==(const BufferUse& other) const
    {
        return stage == other.stage && access == other.access;
    }
};

inline constexpr auto bufferTransferRead =
    BufferUse {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
inline constexpr auto bufferTransferWrite =
    BufferUse {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
inline constexpr auto bufferShaderRead =
    BufferUse {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT};
inline constexpr auto bufferShaderWrite =
    BufferUse {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
inline constexpr auto bufferIndirectRead = BufferUse {
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};

inline constexpr auto bufferVertexRead = BufferUse {
    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
inline constexpr auto bufferIndexRead =
    BufferUse {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};

inline constexpr auto bufferGraphicsRead = BufferUse {
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    VK_ACCESS_2_SHADER_STORAGE_READ_BIT};

// One recording in flight: a command pool and its buffer, plus the transient
// storage its commands reference. Reusable once completionValue has passed.
struct CommandContext
{
    struct UploadChunk
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    // Only sound once the recording's completion value has passed.
    void rewindUploads()
    {
        for (auto& chunk: uploads)
            chunk.used = 0;

        uploadCursor = 0;
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;

    // Zero for a recording that has not been submitted.
    std::uint64_t completionValue = 0;

    VulkanContext* context = nullptr;

    Vector<UploadChunk> uploads;
    int uploadCursor = 0;

    Vector<VkDescriptorPool> descriptorPools;
    int descriptorCursor = 0;

    Vector<int> stagingTaken;
    Vector<int> readbackTaken;
    Vector<int> constantsTaken;

    // A buffer first touched under a new id needs no barrier.
    std::uint64_t recordingId = 0;

    // The images whose deferred initial transition this recording carries, so
    // that discarding it owes them again.
    Vector<VulkanTextureData*> settledImages;
};

// What the driver cannot be asked to do. Empty today.
struct DriverQuirks
{
};

// The binary semaphores a swapchain frame's submission carries. Of a frame that
// flushes, only the first submission waits and only the last signals.
struct SubmitSync
{
    bool isEmpty() const
    {
        return wait == VK_NULL_HANDLE && signal == VK_NULL_HANDLE;
    }

    VkSemaphore wait = VK_NULL_HANDLE;
    VkSemaphore signal = VK_NULL_HANDLE;
};

struct PipelineLayouts
{
    bool isValid() const { return pipelineLayout != VK_NULL_HANDLE; }

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
};

// The process-wide half. Created on first use by getVulkanShared().
class VulkanShared
{
public:
    VulkanShared();
    ~VulkanShared();

    VulkanShared(const VulkanShared&) = delete;
    VulkanShared& operator=(const VulkanShared&) = delete;

    bool isValid() const { return device != VK_NULL_HANDLE; }

    VkInstance getInstance() const { return instance; }
    VkPhysicalDevice getPhysicalDevice() const { return physicalDevice; }
    VkDevice getDevice() const { return device; }
    VkQueue getQueue() const { return queue; }
    std::uint32_t getQueueFamily() const { return queueFamily; }
    VmaAllocator getAllocator() const { return allocator; }

    const std::string& getAdapterName() const { return adapterName; }
    const DriverQuirks& getDriverQuirks() const { return quirks; }

    const VkPhysicalDeviceProperties& getProperties() const { return properties; }
    const VkPhysicalDeviceFeatures& getFeatures() const { return features; }

    bool supportsTimestamps() const { return timestampsSupported; }

    // Whether a multisampled depth image can be resolved for sampling. The spec
    // requires sample zero in both masks, so this only fails on a driver that
    // does not offer the resolve at all.
    bool resolvesDepthBySampleZero() const { return depthResolvesBySampleZero; }

    // Handed to every pipeline creation, and null when the driver refused one.
    VkPipelineCache getPipelineCache() const { return pipelineCache; }

    // Whether the surface and swapchain extensions came up. Says nothing about a
    // particular surface, which vkGetPhysicalDeviceSurfaceSupportKHR answers.
    bool supportsPresentation() const { return presentationSupported; }

    // Nanoseconds per timestamp tick.
    float getTimestampPeriod() const { return properties.limits.timestampPeriod; }

    const PipelineLayouts& getComputeLayouts() const { return computeLayouts; }

    const PipelineLayouts& getRenderLayouts() const { return renderLayouts; }

    // Serialises vkQueueSubmit; the queue is shared by every Device.
    std::mutex& getQueueMutex() { return queueMutex; }

    // Null before the device came up.
    VkSampler getSampler(const TextureSampling& sampling) const
    {
        if (device == VK_NULL_HANDLE)
            return VK_NULL_HANDLE;

        return samplers[samplingIndex(sampling)];
    }

private:
    void createAll();
    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();
    bool createAllocator();
    bool createComputeLayouts();
    bool createRenderLayouts();
    void createDebugMessenger();

    // Both silent: a cache that could not be read, written or created only
    // costs the compile it would have saved.
    void createPipelineCache();
    void savePipelineCache() const;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VmaAllocator allocator = nullptr;

    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceFeatures features = {};
    std::string adapterName = "no Vulkan device";
    DriverQuirks quirks;
    bool timestampsSupported = false;
    bool depthResolvesBySampleZero = false;

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;

    bool surfaceExtensionsEnabled = false;
    bool presentationSupported = false;

    PipelineLayouts computeLayouts;
    PipelineLayouts renderLayouts;

    std::mutex queueMutex;

    // One per samplingIndex, in that order.
    VkSampler samplers[samplingConfigurations] = {};
};

VulkanShared& getVulkanShared();

class VulkanContext
{
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool isValid() const { return timeline != VK_NULL_HANDLE; }

    VkDevice getDevice() const { return getVulkanShared().getDevice(); }
    VmaAllocator getAllocator() const { return getVulkanShared().getAllocator(); }
    VkQueue getQueue() const { return getVulkanShared().getQueue(); }

    // Owned by the caller until submit() or discard(). Callable only from the
    // thread that constructed the Device this context belongs to.
    CommandContext* acquire();

    // The recording an upload joins; its bytes cannot be read back until it
    // submits.
    void setOpenRecording(CommandContext* commands) { openRecording = commands; }
    CommandContext* getOpenRecording() const { return openRecording; }

    // Null while a render pass instance is running, a copy inside
    // vkCmdBeginRendering being illegal.
    CommandContext* getRecordingForCopy() const
    {
        return renderPassOpen ? nullptr : openRecording;
    }

    void setRenderPassOpen(bool open) { renderPassOpen = open; }

    // An image no upload wrote is in UNDEFINED, which nothing may be bound at,
    // so it needs one barrier into its resting layout. Recording it here rather
    // than at creation costs a burst of such textures one submission instead of
    // one each: acquire() drains the queue onto the front of the next recording,
    // which is before anything that recording could bind or copy them.
    void deferImageSettle(VulkanTextureData& data);
    void cancelImageSettle(VulkanTextureData& data);

    // Returns the timeline value the submission signals, zero if nothing was
    // submitted. Same thread rule as acquire().
    std::uint64_t submit(CommandContext* commands, const SubmitSync& sync = {});

    void discard(CommandContext* commands);

    std::uint64_t lastSubmitted() const { return lastSubmittedValue; }

    // For the tests: how many recordings this context has submitted.
    std::uint64_t submissionCount() const { return submissions; }

    bool hasCompleted(std::uint64_t value) const;
    void waitFor(std::uint64_t value);
    void waitIdle();

    // Fires inline if `value` has passed, else from a poll on the event loop.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    ConstantRange uploadConstants(CommandContext& commands,
                                  const void* data,
                                  std::size_t bytes);

    UploadRange allocateUpload(CommandContext& commands, std::size_t bytes);

    // Owned by the pool, and returned to it once `commands` completes.
    VkBuffer acquireStagingBuffer(CommandContext& commands,
                                  std::size_t bytes,
                                  std::byte*& mapped);

    VkBuffer acquireReadbackBuffer(CommandContext& commands,
                                   std::size_t bytes,
                                   std::byte*& mapped);

    // Valid until the recording is recycled.
    VkDescriptorSet allocateDescriptorSet(CommandContext& commands,
                                          VkDescriptorSetLayout layout);

    // Runs `destroy` once the GPU is done and no recording is open.
    void deferRelease(Callback destroy);

    void deferReleaseBuffer(VkBuffer buffer, VmaAllocation allocation);

    // Follows the main thread rather than the one that constructed this.
    void followMainThread() { mainThreadOwned = true; }

private:
    struct ConstantPage
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        std::size_t bytes = 0;
        std::size_t used = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;

        std::size_t remaining() const { return bytes - used; }
    };

    // `freeAt` is the value that must pass before the slot can be lent again;
    // `lent` covers the window before submit stamps that value.
    struct PooledBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        std::size_t bytes = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;
    };

    // `stamped` marks the ones a completion value has been worked out for.
    struct Retired
    {
        Callback destroy;
        std::uint64_t completionValue = 0;
        bool stamped = false;
    };

    struct PendingCompletion
    {
        std::uint64_t completionValue = 0;
        Callback done;
    };

    void createAll();
    void releaseAll();
    void purgeRetired();
    void pollCompletions();
    void reportFailedRecording() const;

    void assertOwningThread() const;

    void recordDeferredSettles(CommandContext& commands);
    void requeueDeferredSettles(CommandContext& commands);

    CommandContext::UploadChunk* uploadRoomFor(CommandContext& commands,
                                               std::size_t bytes);

    ConstantPage* pageFor(CommandContext& commands, std::size_t bytes);

    // Mapped for its whole life.
    bool makeHostBuffer(std::size_t bytes,
                        VkBufferUsageFlags usage,
                        bool readBack,
                        VkBuffer& buffer,
                        VmaAllocation& allocation,
                        std::byte*& mapped);

    VkBuffer acquirePooled(Vector<PooledBuffer>& pool,
                           Vector<int>& taken,
                           std::size_t bytes,
                           VkBufferUsageFlags usage,
                           bool readBack,
                           std::byte*& mapped);

    void returnPooled(Vector<PooledBuffer>& pool,
                      Vector<int>& taken,
                      std::uint64_t freeAt);

    // `freeAt` is 0 for a recording that never reached the GPU.
    void returnStaging(CommandContext& commands, std::uint64_t freeAt);
    void returnConstantPages(CommandContext& commands, std::uint64_t freeAt);

    void destroyPool(Vector<PooledBuffer>& pool);

    std::uint64_t owningThreadId = 0;
    bool mainThreadOwned = false;

    // Signalled by every submit, one value higher each time.
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t nextValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t submissions = 0;
    std::uint64_t recordingCounter = 0;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;
    CommandContext* openRecording = nullptr;
    bool renderPassOpen = false;

    // Only ever non-empty while no recording is open, so nothing can bind an
    // image that is still waiting here.
    Vector<VulkanTextureData*> pendingSettles;

    Vector<ConstantPage> constantPages;
    Vector<PooledBuffer> staging;
    Vector<PooledBuffer> readback;
    Vector<Retired> retired;

    static constexpr int completionPollHz = 240;

    Vector<PendingCompletion> pendingCompletions;
    std::optional<Threads::Timer> completionPoll;
};

// A resource does not cross Devices.
VulkanContext& getVulkanContext(const Device& device);
} // namespace eacp::GPU
