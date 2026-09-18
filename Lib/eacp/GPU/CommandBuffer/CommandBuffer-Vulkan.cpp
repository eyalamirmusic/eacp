#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"
#include "../Vulkan/VulkanBackend-Linux.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
struct VulkanCommandBufferBackend final : CommandBufferBackend
{
    explicit VulkanCommandBufferBackend(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getVulkanContext(deviceToUse))
    {
        if (context.isValid())
            open(context.acquire());
    }

    ~VulkanCommandBufferBackend() override
    {
        close();

        if (commands != nullptr && !committed)
            context.discard(commands);
    }

    // Publishes the recording as the one a CPU upload may record onto.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            context.setOpenRecording(commands);
    }

    // Withdrawn before the submit, so an upload never gets an ended buffer.
    void close()
    {
        if (commands != nullptr && context.getOpenRecording() == commands)
            context.setOpenRecording(nullptr);
    }

    // The pass's own pair of timestamps, as Frame::timePass writes them.
    void timePass(VulkanComputeEncoder& encoder, std::string_view label)
    {
        const auto pass = timer.beginPass(label, *device, commands->buffer);

        if (pass < 0)
            return;

        auto queryPool = static_cast<VkQueryPool>(timer.nativeSamples());

        if (queryPool == VK_NULL_HANDLE)
            return;

        vkCmdWriteTimestamp2(commands->buffer,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             queryPool,
                             static_cast<std::uint32_t>(pass * 2));

        encoder.queryPool = queryPool;
        encoder.endQuery = pass * 2 + 1;
    }

    bool canSubmit() const { return commands != nullptr && !committed; }

    // Everything a submission needs recorded on it, in the order it needs it.
    // The timeline value is kept, which is what scopes wait() and isComplete()
    // to this submission rather than to the newest one on the queue.
    void endAndSubmit()
    {
        committed = true;
        close();

        timer.endRecording(commands->buffer);

        completionValue = context.submit(commands);
        timer.noteSubmitted(completionValue);
    }

    bool isValid() const override { return commands != nullptr; }

    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view label,
                                                     DispatchOrder order) override
    {
        device->assertOwningThread();

        if (commands == nullptr || committed)
            return nullptr;

        auto* encoder = new VulkanComputeEncoder {commands};
        timePass(*encoder, label);

        return makeVulkanComputePass(encoder, order);
    }

    void fill(const BufferRange& range, std::uint8_t value) override
    {
        if (commands == nullptr || committed)
            return;

        auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE)
            return;

        const auto offset = static_cast<VkDeviceSize>(range.offset);

        // vkCmdFillBuffer takes a word, so both ends are on the four-byte grid
        // the range's own contract already asks for.
        if (offset % 4 != 0)
            return;

        const auto available = data->size - static_cast<std::size_t>(range.offset);
        const auto wanted = static_cast<std::size_t>(range.bytes);
        const auto length =
            (wanted < available ? wanted : available) & ~std::size_t {3};

        if (length == 0)
            return;

        transitionForUse(*commands, *data, bufferTransferWrite);

        const auto word = static_cast<std::uint32_t>(value);

        vkCmdFillBuffer(commands->buffer,
                        data->buffer,
                        offset,
                        static_cast<VkDeviceSize>(length),
                        word | (word << 8) | (word << 16) | (word << 24));
    }

    void submit() override
    {
        device->assertOwningThread();

        if (canSubmit())
            endAndSubmit();
    }

    void wait() override
    {
        device->assertOwningThread();

        if (committed)
            context.waitFor(completionValue);
    }

    bool isComplete() const override
    {
        return committed && context.hasCompleted(completionValue);
    }

    Threads::Async<void> commitAsync() override
    {
        // The submission is the part that belongs to this thread; the completion
        // handler below hops to the message thread on its own and asserts
        // nothing.
        device->assertOwningThread();

        auto promise = Threads::AsyncPromise<void> {};

        if (!canSubmit())
        {
            promise.resolve();
            return promise.get();
        }

        // The callback holds the promise's own shared state and nothing of this
        // object, so a CommandBuffer destroyed while the poll is outstanding
        // leaves nothing dangling.
        endAndSubmit();

        context.notifyWhenCompleted(completionValue,
                                    [promise] { promise.resolve(); });

        return promise.get();
    }

    const FrameTimings& timings() override { return timer.timings(*device); }

    bool supportsPassTimings() const override { return timer.isSupported(); }

    Device* device = nullptr;
    VulkanContext& context;
    CommandContext* commands = nullptr;
    CommandTimer timer;
    std::uint64_t completionValue = 0;
    bool committed = false;
};
} // namespace

std::unique_ptr<CommandBufferBackend> makeVulkanCommandBuffer(Device& device)
{
    return std::make_unique<VulkanCommandBufferBackend>(device);
}
} // namespace eacp::GPU
