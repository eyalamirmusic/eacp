#include <eacp/Core/Utils/WinInclude.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"
#include "../Windows/D3D12Types.h"

#include <cstring>

// Windows/D3D12 backend. Owns one CommandContext recording for its lifetime:
// passes record onto its list, commit() executes it on the direct queue, and
// an uncommitted recording is discarded on destruction. The fence wait inside
// Buffer::read serialises behind the committed work, so a read after commit
// sees the kernel's output.

namespace eacp::GPU
{
namespace
{
// The pattern a fill copies from, repeated until the range is covered - so a
// large fill borrows this much of the recording's upload arena and no more.
constexpr std::size_t fillPatternBytes = 64 * 1024;
} // namespace

struct CommandBuffer::Native
{
    explicit Native(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getD3D12Context(deviceToUse))
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

    // Publishes the recording as the one a CPU upload may record onto, for as
    // long as this command buffer is the thing recording - the same courtesy
    // Frame extends, and for the same reason. A buffer filled between here and
    // commit() puts its copy on this list instead of acquiring and submitting
    // one of its own, so a batch that uploads seven buffers before dispatching
    // them is one submission rather than eight.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            context.setOpenRecording(commands);
    }

    // Withdrawn before anything is submitted, so an upload can never be handed
    // a list that has already been closed.
    void close()
    {
        if (commands != nullptr && context.getOpenRecording() == commands)
            context.setOpenRecording(nullptr);
    }

    Device* device = nullptr;
    D3D12Context& context;
    CommandContext* commands = nullptr;
    CommandTimer timer;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label)
{
    if (impl->commands == nullptr || impl->committed)
        return ComputePass(nullptr);

    auto* list = impl->commands->list.get();

    // The root signature and heaps are fixed for every compute pipeline, so
    // binding them here frees the pass from caring about setPipeline/set*
    // ordering.
    bindComputeRootState(impl->context, list);

    auto* encoder = new D3D12ComputeEncoder {impl->commands};

    const auto pass = impl->timer.beginPass(label, *impl->device, list);

    if (pass >= 0)
    {
        if (auto* heap = static_cast<ID3D12QueryHeap*>(impl->timer.nativeSamples()))
        {
            list->EndQuery(
                heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(pass * 2));

            encoder->queryHeap = heap;
            encoder->endQuery = pass * 2 + 1;
        }
    }

    return ComputePass(encoder);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    if (impl->commands == nullptr || impl->committed || !range.isValid()
        || range.bytes <= 0 || range.offset < 0
        || range.offset >= range.buffer->size())
        return;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());

    // An upload-heap buffer holds bytes only the CPU ever writes, and may not
    // leave GENERIC_READ to be copied into.
    if (data == nullptr || data->resource == nullptr || data->uploadHeap)
        return;

    const auto available = (std::size_t) (range.buffer->size() - range.offset);
    const auto length = (std::size_t) range.bytes < available
                            ? (std::size_t) range.bytes
                            : available;

    auto& commands = *impl->commands;
    const auto patternBytes = length < fillPatternBytes ? length : fillPatternBytes;

    auto pattern = impl->context.allocateUpload(commands, patternBytes);

    if (!pattern.isValid())
        return;

    std::memset(pattern.mapped, value, patternBytes);

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_COPY_DEST);

    for (std::size_t written = 0; written < length; written += patternBytes)
    {
        const auto remaining = length - written;
        const auto step = remaining < patternBytes ? remaining : patternBytes;

        commands.list->CopyBufferRegion(data->resource.get(),
                                        static_cast<UINT64>(range.offset) + written,
                                        pattern.resource,
                                        pattern.offset,
                                        step);
    }
}

void CommandBuffer::commit()
{
    if (impl->commands == nullptr || impl->committed)
        return;

    impl->committed = true;
    impl->close();

    // Waits, because Metal's commit does ([buffer waitUntilCompleted]) and one
    // contract has to hold on both backends. Without it they disagree on what a
    // returned commit() means: code that commits and then reads its results
    // through anything but Buffer::read - which waits on its own fence - would
    // race here and not there, and a benchmark timing commit() would measure
    // the CPU-side record on this backend and the finished work on that one.
    // commitAsync() is how a caller opts out of the wait.
    auto& context = impl->context;

    impl->timer.endRecording(impl->commands->list.get());

    const auto fenceValue = context.submit(impl->commands);
    impl->timer.noteSubmitted(fenceValue);

    context.waitFor(fenceValue);
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    auto promise = Threads::AsyncPromise<void> {};

    if (impl->commands == nullptr || impl->committed)
    {
        promise.resolve();
        return promise.get();
    }

    impl->committed = true;
    impl->close();

    // submit() already returns without waiting here - what the fence adds is
    // the moment to say so.
    auto& context = impl->context;

    impl->timer.endRecording(impl->commands->list.get());

    const auto fenceValue = context.submit(impl->commands);
    impl->timer.noteSubmitted(fenceValue);

    context.notifyWhenCompleted(fenceValue, [promise] { promise.resolve(); });

    return promise.get();
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
