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

    bool canSubmit() const { return commands != nullptr && !committed; }

    // Everything a submission needs recorded on it, in the order it needs it.
    // The fence value is kept, which is what scopes wait() and isComplete() to
    // this submission rather than to the newest one on the queue.
    void endAndSubmit()
    {
        committed = true;
        close();

        timer.endRecording(commands->list.get());

        completionValue = context.submit(commands);
        timer.noteSubmitted(completionValue);
    }

    Device* device = nullptr;
    D3D12Context& context;
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

    return ComputePass(encoder, order);
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

void CommandBuffer::submit()
{
    if (impl->canSubmit())
        impl->endAndSubmit();
}

void CommandBuffer::commit()
{
    // Waits, because Metal's commit does ([buffer waitUntilCompleted]) and one
    // contract has to hold on both backends. Without it they disagree on what a
    // returned commit() means: code that commits and then reads its results
    // through anything but Buffer::read - which waits on its own fence - would
    // race here and not there, and a benchmark timing commit() would measure
    // the CPU-side record on this backend and the finished work on that one.
    // submit() and commitAsync() are how a caller opts out of the wait.
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

    // The context's submit already returns without waiting here - what the
    // fence adds is the moment to say so. The callback holds the promise's own
    // shared state and nothing of this object, so a CommandBuffer destroyed
    // while the poll is outstanding leaves nothing dangling.
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
// submitted in between. That costs a pipelined loop here what it saves on
// Metal, and is the price of a default-heap buffer having no CPU mapping to
// memcpy out of.
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
