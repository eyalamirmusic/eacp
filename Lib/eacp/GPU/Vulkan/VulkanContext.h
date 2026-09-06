#pragma once

#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/Containers.h>

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

// The Vulkan plumbing shared by every Linux GPU translation unit, split in two
// along the same line the D3D12 backend draws (Windows/D3D12Context.h), which
// is the line that decides what a second GPU::Device can be.
//
// VulkanShared is one per process: the loader, the VkInstance, the
// VkPhysicalDevice, the VkDevice, the queue, the VMA allocator and the
// descriptor-set and pipeline layouts. All of it is immutable once created or
// free-threaded by contract, and duplicating any of it would cost a second
// driver device.
//
// VulkanContext is one per GPU::Device: the command pool ring, the timeline
// semaphore, the upload arena, the constant ring, the staging and readback
// pools and the deferred-release list. None of that is synchronized, which is
// exactly why it cannot be shared - two Devices forwarding to one context are
// two handles to the same pool, and the second one's recording lands in the
// first one's command buffer. A Device is therefore single-threaded and owns
// its context; see the affinity note on acquire().
//
// The one thing that does not follow D3D12 is the queue. A D3D12 command queue
// is created on demand, so a Device makes its own; a VkQueue is handed out of a
// family with a fixed count decided by the driver, and Mesa's lavapipe - the CI
// device - offers exactly one. So the queue lives in the shared half and every
// vkQueueSubmit goes through one mutex. Everything a Device needs to be
// independent still is: its own pools, its own timeline, its own rings, and
// waits that name only its own submissions.
//
// Not part of GPU.h.

namespace eacp::GPU
{
class Device;
class VulkanContext;

// A range of a recording's upload arena: the buffer a copy sources from, where
// the CPU writes and how far into the buffer that is. Valid until the recording
// it came from has completed on the GPU.
struct UploadRange
{
    bool isValid() const { return mapped != nullptr; }

    VkBuffer buffer = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
    VkDeviceSize offset = 0;
};

// A block of the constant ring, as a UNIFORM_BUFFER_DYNAMIC binds one: the
// descriptor names the buffer and the range, and the offset is the dynamic
// offset handed to vkCmdBindDescriptorSets.
//
// `range` is the block rounded up rather than its byte count. std140 rounds a
// block to a multiple of 16 where the CPU packs it to the widest member, so a
// uniform struct ending on a float is four bytes shorter here than the shader
// believes it is - and a range shorter than the block the shader declares is a
// validation error at the dispatch.
struct ConstantRange
{
    bool isValid() const { return buffer != VK_NULL_HANDLE; }

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = 0;
};

// What a buffer is being used as, for the barrier that has to precede it. One
// combined {stage, access} pair rather than two, because every use eacp makes
// of a buffer pins both together.
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

// One recording in flight: a command pool and its buffer, plus the transient
// storage - upload chunks, descriptor sets, pooled staging slots - that the
// recorded commands reference. All of it is reusable again once
// completionValue has passed on the timeline.
struct CommandContext
{
    // Where everything this recording uploads from the CPU is bump-allocated: a
    // host-visible buffer left mapped for its whole life, handed out a range at
    // a time. A buffer's vkCmdCopyBuffer sources from it.
    //
    // One buffer per *recording* rather than per upload, for the reason
    // D3D12's arena exists: a frame uploads a couple of hundred times between
    // its uniforms and its instance data, and an allocation each would be by
    // far the most expensive thing it does. A chain rather than one buffer
    // because a recording that outgrows the chunk cannot be given a larger one
    // - the commands already recorded name the old buffer - so a second is
    // added and both are kept, which also means the chain settles at the peak a
    // frame actually uses and then stops growing.
    struct UploadChunk
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    // Ready to be filled from the start again. Only sound once the recording's
    // completion value has passed, which is the same condition that lets the
    // command pool be reset - the GPU has read everything these chunks carried.
    void rewindUploads()
    {
        for (auto& chunk: uploads)
            chunk.used = 0;

        uploadCursor = 0;
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;

    // The value on this context's timeline that the submission signals. Zero
    // for a recording that has not been submitted.
    std::uint64_t completionValue = 0;

    // The context that lent this recording out, so anything holding one - an
    // encoder, a pass - reaches the queue, the constant ring and the descriptor
    // pools it belongs to without being told which Device it came from.
    VulkanContext* context = nullptr;

    Vector<UploadChunk> uploads;
    int uploadCursor = 0;

    // Descriptor pools this recording allocates its sets out of, oldest first,
    // so the last entry is the one still being drawn from. Reset together when
    // the recording is recycled, which is why they are per recording rather
    // than per pass: the reset condition is the completion value, and that is
    // what a recording is identified by.
    Vector<VkDescriptorPool> descriptorPools;
    int descriptorCursor = 0;

    // Staging-pool slots this recording is copying out of, and readback slots
    // it is copying into. Unlike the upload chunks these are not owned by the
    // recording - they go back to their pools, stamped at submit with the value
    // that frees them again.
    Vector<int> stagingTaken;
    Vector<int> readbackTaken;

    // Constant-ring pages this recording is bump-allocating from, on the same
    // terms.
    Vector<int> constantsTaken;

    // Identifies the recording for buffer use tracking: a buffer first touched
    // under a new id needs no barrier, the previous recording having ended with
    // a global one. See VulkanContext::submit.
    std::uint64_t recordingId = 0;
};

// What the driver cannot be asked to do, found out by asking it. Empty on
// Vulkan today and deliberately kept: the D3D12 backend found two shipping
// drivers that refuse a spec-legal operation by killing the command list rather
// than by returning an error, and this is where the same discovery lands here.
// Nothing is added on suspicion - a quirk goes in when it has been reproduced.
struct DriverQuirks
{
};

// A descriptor-set layout and the pipeline layout over it, which are made
// together and always used together.
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

    // False when there is no loader, no device that meets the feature floor, or
    // no queue - each of which is a normal thing for a machine to be, and each
    // of which reaches an app as Device::isValid() being false rather than as a
    // crash. See the feature list in VulkanContext-Linux.cpp.
    bool isValid() const { return device != VK_NULL_HANDLE; }

    VkInstance getInstance() const { return instance; }
    VkPhysicalDevice getPhysicalDevice() const { return physicalDevice; }
    VkDevice getDevice() const { return device; }
    VkQueue getQueue() const { return queue; }
    std::uint32_t getQueueFamily() const { return queueFamily; }
    VmaAllocator getAllocator() const { return allocator; }

    // What the device calls itself, read once at creation. Shared rather than
    // per-context because every Device in the process runs on it.
    const std::string& getAdapterName() const { return adapterName; }
    const DriverQuirks& getDriverQuirks() const { return quirks; }

    const VkPhysicalDeviceProperties& getProperties() const { return properties; }
    const VkPhysicalDeviceFeatures& getFeatures() const { return features; }

    // Whether the device writes timestamps at all, and on this queue. Both have
    // to be true before GpuTimestamps builds anything.
    bool supportsTimestamps() const { return timestampsSupported; }

    // Nanoseconds per timestamp tick, straight off the device limits.
    float getTimestampPeriod() const { return properties.limits.timestampPeriod; }

    // The layouts a compute pipeline is built against and a compute pass binds
    // through, laid out from Codegen/ShaderBindings.h so a kernel's descriptor
    // set and the source the emitter wrote cannot drift apart.
    const PipelineLayouts& getComputeLayouts() const { return computeLayouts; }

    // Serialises vkQueueSubmit. One queue is shared by every Device (see the
    // note at the top), and a VkQueue is externally synchronized.
    std::mutex& getQueueMutex() { return queueMutex; }

private:
    void createAll();
    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();
    bool createAllocator();
    bool createComputeLayouts();
    void createDebugMessenger();

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

    PipelineLayouts computeLayouts;

    std::mutex queueMutex;
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

    // Forwarded from the shared half, so a call site holding a context reaches
    // the device and the allocator the same way it reaches its own pools.
    VkDevice getDevice() const { return getVulkanShared().getDevice(); }
    VmaAllocator getAllocator() const { return getVulkanShared().getAllocator(); }
    VkQueue getQueue() const { return getVulkanShared().getQueue(); }

    // An open command buffer ready for recording. Owned by the caller until it
    // is handed back through submit() or discard().
    //
    // Callable only from the thread that constructed the Device this context
    // belongs to - nothing in here is synchronized, and the whole point of a
    // context per Device is that it needs no lock. The assertion is the
    // enforcement: a violation is a loud debug failure rather than a pool
    // handing the same recording to two threads.
    CommandContext* acquire();

    // The recording a frame or command buffer currently has open, or null
    // outside one. A CPU upload that happens while one is being recorded puts
    // its copy on that command buffer instead of acquiring and submitting one
    // of its own: same queue, in order ahead of the dispatch that wanted the
    // bytes, and no submission per buffer.
    //
    // The one thing it is not safe for is a read-back of bytes uploaded this
    // way before the recording submits - see Buffer::read.
    void setOpenRecording(CommandContext* commands) { openRecording = commands; }
    CommandContext* getOpenRecording() const { return openRecording; }

    // Ends and submits the command buffer, signalling the next value on this
    // context's timeline, and recycles the recording. Returns the value that
    // completes when the GPU has finished. Same thread rule as acquire().
    std::uint64_t submit(CommandContext* commands);

    // Recycles a recording that should never reach the GPU.
    void discard(CommandContext* commands);

    std::uint64_t lastSubmitted() const { return lastSubmittedValue; }
    bool hasCompleted(std::uint64_t value) const;
    void waitFor(std::uint64_t value);
    void waitIdle();

    // Calls `done` once `value` has passed on the GPU, without blocking - the
    // non-blocking sibling of waitFor, and what CommandBuffer::commitAsync is
    // built on. Fires inline when the value has already passed; otherwise from
    // a poll on the event loop, so `done` always runs on the thread that owns
    // this context - which must therefore be a thread running an event loop. A
    // worker Device without one commits synchronously instead.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    // Copies bytes into the recording's constant ring and hands back what a
    // UNIFORM_BUFFER_DYNAMIC descriptor and its offset need. The page outlives
    // GPU execution, being held by the recording until its value passes.
    ConstantRange uploadConstants(CommandContext& commands,
                                  const void* data,
                                  std::size_t bytes);

    // Room for `bytes` on the recording's upload arena, for a caller that
    // copies out of it rather than having the GPU read it in place - the source
    // of a buffer or texture upload. Invalid on failure.
    UploadRange allocateUpload(CommandContext& commands, std::size_t bytes);

    // A host-visible buffer of at least `bytes`, borrowed from a pool and
    // returned once `commands` completes on the GPU. Null on failure.
    //
    // For staging that repeats every frame at a size worth pooling - a video
    // frame is a 33 MB upload at 4K - where the allocation costs more than the
    // copy it exists for. The caller must not release the result; the pool owns
    // it. `mapped` receives the pointer to write the upload into.
    VkBuffer acquireStagingBuffer(CommandContext& commands,
                                  std::size_t bytes,
                                  std::byte*& mapped);

    // The download-side sibling, out of a pool of host-visible readback buffers
    // and on identical terms. Buffer::read stages every download through one.
    // `mapped` receives the pointer to read out of once the GPU is done.
    VkBuffer acquireReadbackBuffer(CommandContext& commands,
                                   std::size_t bytes,
                                   std::byte*& mapped);

    // A descriptor set of `layout` out of this recording's pools, adding a pool
    // when the open one is full. Valid until the recording is recycled, which
    // is what makes a set per dispatch affordable. Null on failure.
    VkDescriptorSet allocateDescriptorSet(CommandContext& commands,
                                          VkDescriptorSetLayout layout);

    // Keeps an object alive until the GPU has finished all work submitted so
    // far and no recording is still open, then runs `destroy`. Buffer, pipeline
    // and shader-module destructors route their objects through here rather
    // than releasing something a command buffer still names.
    //
    // A callback rather than a handle because the kinds do not share a destroy
    // call, and a variant of eight of them would be longer than this is.
    void deferRelease(Callback destroy);

    void deferReleaseBuffer(VkBuffer buffer, VmaAllocation allocation);

    // Makes this context follow the main thread rather than the one that
    // constructed it. Device::shared() is the only caller: it is created
    // lazily, so a worker that merely touched it first - to compile a kernel,
    // say - would otherwise own the process-wide Device, and the UI's next
    // frame would trip the assertion in acquire().
    void followMainThread() { mainThreadOwned = true; }

private:
    // A page of the constant ring: a host-visible buffer mapped once for its
    // whole lifetime and bump-allocated from, one uniform block at a time.
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

    // A slot in one of the host-visible pools. `freeAt` is the value that must
    // pass before it can be lent out again; `lent` marks the window between the
    // acquire and the submit that stamps the real value, during which the slot
    // must not be handed to a second recording.
    struct PooledBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::byte* mapped = nullptr;
        std::size_t bytes = 0;
        std::uint64_t freeAt = 0;
        bool lent = false;
    };

    // An object whose owner is gone but which a command buffer may still name.
    // `stamped` marks the ones a completion value has been worked out for;
    // until then there is no bound on when they are free. See purgeRetired.
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

    // The thread rule acquire() and submit() document. Compiled out of a
    // release build, where the whole check is the assertion.
    void assertOwningThread() const;

    // A chunk of this recording's upload arena with room for `bytes`, adding
    // one to the chain if nothing already there has it. Null on failure.
    CommandContext::UploadChunk* uploadRoomFor(CommandContext& commands,
                                               std::size_t bytes);

    // The page `commands` can fit `bytes` into, taking a fresh one from the
    // ring when the open one is full. Null if none can be had.
    ConstantPage* pageFor(CommandContext& commands, std::size_t bytes);

    // A host-visible buffer of `bytes` with these usage bits, mapped for its
    // whole life. The shared body of the arena, the ring and both pools.
    bool makeHostBuffer(std::size_t bytes,
                        VkBufferUsageFlags usage,
                        bool readBack,
                        VkBuffer& buffer,
                        VmaAllocation& allocation,
                        std::byte*& mapped);

    // The pool logic both acquire calls are: reuse a free slot that already
    // fits, else grow one, else add one.
    VkBuffer acquirePooled(Vector<PooledBuffer>& pool,
                           Vector<int>& taken,
                           std::size_t bytes,
                           VkBufferUsageFlags usage,
                           bool readBack,
                           std::byte*& mapped);

    void returnPooled(Vector<PooledBuffer>& pool,
                      Vector<int>& taken,
                      std::uint64_t freeAt);

    // Hands a recording's pooled slots, both directions, back to their pools.
    // `freeAt` is the value that must pass before they can be lent out again -
    // 0 for a recording that never reached the GPU, so its slots are free at
    // once.
    void returnStaging(CommandContext& commands, std::uint64_t freeAt);
    void returnConstantPages(CommandContext& commands, std::uint64_t freeAt);

    void destroyPool(Vector<PooledBuffer>& pool);

    // The thread that constructed this context, and therefore the one Device it
    // belongs to may be used from. Stamped once and never changed - unless
    // followMainThread() said to track the main thread instead.
    std::uint64_t owningThreadId = 0;
    bool mainThreadOwned = false;

    // Signalled by every submit, one value higher each time. The whole of this
    // context's synchronization: hasCompleted is a counter read, waitFor is a
    // wait on a value, and a Device waits only for its own submissions because
    // only its own submissions ever signal this.
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t nextValue = 1;
    std::uint64_t lastSubmittedValue = 0;
    std::uint64_t recordingCounter = 0;

    OwnedVector<CommandContext> pool;
    Vector<CommandContext*> available;
    CommandContext* openRecording = nullptr;

    Vector<ConstantPage> constantPages;
    Vector<PooledBuffer> staging;
    Vector<PooledBuffer> readback;
    Vector<Retired> retired;

    static constexpr int completionPollHz = 240;

    Vector<PendingCompletion> pendingCompletions;
    std::optional<Threads::Timer> completionPoll;
};

// The context belonging to a Device - its command pools, its timeline, its
// rings.
//
// A resource does not cross Devices: a buffer or a recording belongs to the
// context that made it, and purgeRetired's correctness argument (that the
// context is *fully* idle) holds only per context. That is Metal's contract
// anyway, an MTLBuffer belonging to its MTLDevice.
VulkanContext& getVulkanContext(const Device& device);
} // namespace eacp::GPU
