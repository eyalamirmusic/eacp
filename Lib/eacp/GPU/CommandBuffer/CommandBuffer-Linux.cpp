#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getVulkanContext(deviceToUse))
    {
        if (context.isValid())
            open(context.acquire());
    }

    ~Native()
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

    Device* device = nullptr;
    VulkanContext& context;
    CommandContext* commands = nullptr;
    CommandTimer timer;
    std::uint64_t completionValue = 0;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label, DispatchOrder order)
{
    if (impl->commands == nullptr || impl->committed)
        return ComputePass(nullptr, order);

    auto* encoder = new VulkanComputeEncoder {impl->commands};
    impl->timePass(*encoder, label);

    return ComputePass(encoder, order);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    if (impl->commands == nullptr || impl->committed || !range.isValid()
        || range.bytes <= 0 || range.offset < 0
        || range.offset >= range.buffer->size())
        return;

    auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE)
        return;

    const auto offset = static_cast<VkDeviceSize>(range.offset);

    // vkCmdFillBuffer takes a word, so both ends are on the four-byte grid the
    // range's own contract already asks for.
    if (offset % 4 != 0)
        return;

    const auto available = data->size - static_cast<std::size_t>(range.offset);
    const auto wanted = static_cast<std::size_t>(range.bytes);
    const auto length = (wanted < available ? wanted : available) & ~std::size_t {3};

    if (length == 0)
        return;

    transitionForUse(*impl->commands, *data, bufferTransferWrite);

    const auto word = static_cast<std::uint32_t>(value);

    vkCmdFillBuffer(impl->commands->buffer,
                    data->buffer,
                    offset,
                    static_cast<VkDeviceSize>(length),
                    word | (word << 8) | (word << 16) | (word << 24));
}

void CommandBuffer::submit()
{
    if (impl->canSubmit())
        impl->endAndSubmit();
}

void CommandBuffer::commit()
{
    // Waits, as Metal's commit does; submit() and commitAsync() are how a
    // caller opts out.
    submit();
    wait();
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    auto promise = Threads::AsyncPromise<void> {};

    if (!impl->canSubmit())
    {
        promise.resolve();
        return promise.get();
    }

    // The callback holds the promise's own shared state and nothing of this
    // object, so a CommandBuffer destroyed while the poll is outstanding
    // leaves nothing dangling.
    impl->endAndSubmit();

    impl->context.notifyWhenCompleted(impl->completionValue,
                                      [promise] { promise.resolve(); });

    return promise.get();
}

void CommandBuffer::wait()
{
    if (impl->committed)
        impl->context.waitFor(impl->completionValue);
}

bool CommandBuffer::isComplete() const
{
    return impl->committed && impl->context.hasCompleted(impl->completionValue);
}

// The wait is scoped to this submission; the copy after it is not, the queue
// being in order, so a readback recorded now still runs behind whatever was
// submitted in between - the same deal the D3D12 backend gets, and for the
// same reason.
void CommandBuffer::read(const Buffer& buffer, void* dst, int bytes, int offset)
{
    wait();
    buffer.read(dst, bytes, offset);
}

const FrameTimings& CommandBuffer::timings()
{
    return impl->timer.timings(*impl->device);
}

bool CommandBuffer::supportsPassTimings() const
{
    return impl->timer.isSupported();
}

bool CommandBuffer::isValid() const
{
    return impl->commands != nullptr;
}
} // namespace eacp::GPU
