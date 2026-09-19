#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "../Timing/CommandTimer.h"

#include <cstring>
#include <vector>

namespace eacp::GPU
{
namespace
{
// A GL context is its own stream, so what a command buffer records is already
// recorded by the calls that made it. What is left for this to own is the fence
// that says when the stream has run: submit plants one, wait blocks on it, and
// isComplete asks.
struct GLCommandBufferBackend final : CommandBufferBackend
{
    explicit GLCommandBufferBackend(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getGLContext(deviceToUse))
    {
    }

    ~GLCommandBufferBackend() override
    {
        if (encoder.fence == nullptr || !context.isValid())
            return;

        context.makeCurrent();
        glDeleteSync(encoder.fence);
    }

    bool isValid() const override { return context.isValid(); }

    // Stage 6's tier, and a null pass is what the portable half drops every
    // dispatch under (D9).
    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view,
                                                     DispatchOrder) override
    {
        return nullptr;
    }

    void fill(const BufferRange& range, std::uint8_t value) override
    {
        if (!context.isValid() || encoder.committed)
            return;

        auto* data = static_cast<GLBufferData*>(range.buffer->nativeBuffer());

        if (data == nullptr || !data->isValid())
            return;

        // The four-byte grid the range's own contract asks for, and the grid a
        // word-wise clear needs either way.
        if (range.offset % 4 != 0)
            return;

        const auto available = data->size - range.offset;
        const auto wanted = range.bytes;
        const auto length = (wanted < available ? wanted : available) & ~(3LL);

        if (length <= 0)
            return;

        context.makeCurrent();

        const auto word = (std::uint32_t) value;
        const auto filled = word | (word << 8) | (word << 16) | (word << 24);

        // GL_COPY_WRITE_BUFFER, which no draw reads: filling through the usage
        // target would knock out a binding a pass had set.
        glBindBuffer(GL_COPY_WRITE_BUFFER, data->buffer);

        if (glad_glClearBufferSubData != nullptr)
            glClearBufferSubData(GL_COPY_WRITE_BUFFER,
                                 GL_R32UI,
                                 (GLintptr) range.offset,
                                 (GLsizeiptr) length,
                                 GL_RED_INTEGER,
                                 GL_UNSIGNED_INT,
                                 &filled);
        else
            fillByHand(*data, range.offset, length, filled);

        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDrainErrors("buffer fill");
    }

    // The floor path: below core 4.3 there is no clear at all, so the words are
    // made on the CPU and handed over in one write.
    void fillByHand(const GLBufferData& data,
                    std::int64_t offset,
                    std::int64_t length,
                    std::uint32_t word)
    {
        const auto words = (std::size_t) (length / 4);

        if (data.mapped != nullptr)
        {
            auto* out = data.mapped + offset;

            for (auto i = std::size_t {0}; i < words; ++i)
                std::memcpy(out + i * 4, &word, 4);

            return;
        }

        scratch.assign(words, word);

        glBufferSubData(GL_COPY_WRITE_BUFFER,
                        (GLintptr) offset,
                        (GLsizeiptr) length,
                        scratch.data());
    }

    void submit() override
    {
        device->assertOwningThread();

        if (!context.isValid() || encoder.committed)
            return;

        context.makeCurrent();

        timer.endRecording(nullptr);

        encoder.committed = true;
        encoder.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        glFlush();

        timer.noteSubmitted(1);
    }

    void wait() override
    {
        device->assertOwningThread();

        if (!encoder.committed || encoder.fence == nullptr)
            return;

        context.makeCurrent();
        glClientWaitSync(
            encoder.fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    }

    bool isComplete() const override
    {
        if (!encoder.committed)
            return false;

        if (encoder.fence == nullptr)
            return true;

        context.makeCurrent();

        return glClientWaitSync(encoder.fence, 0, 0) != GL_TIMEOUT_EXPIRED;
    }

    // The submission is this thread's; the completion is polled off the message
    // thread, as the Vulkan backend's is, and holds nothing of this object.
    Threads::Async<void> commitAsync() override
    {
        device->assertOwningThread();

        auto promise = Threads::AsyncPromise<void> {};

        if (!context.isValid() || encoder.committed)
        {
            promise.resolve();
            return promise.get();
        }

        submit();

        // A fence of the poll's own, deleted by it: the one submit() planted
        // goes with this object, which may be gone by the time the message
        // thread comes round.
        auto* fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        auto* contextToPoll = &context;

        glFlush();

        Threads::callAsync(
            [promise, fence, contextToPoll]
            {
                if (fence == nullptr)
                {
                    promise.resolve();
                    return;
                }

                contextToPoll->makeCurrent();
                glClientWaitSync(
                    fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
                glDeleteSync(fence);

                promise.resolve();
            });

        return promise.get();
    }

    const FrameTimings& timings() override { return timer.timings(*device); }

    bool supportsPassTimings() const override { return timer.isSupported(); }

    Device* device = nullptr;
    GLContext& context;

    GLCommandEncoder encoder;
    CommandTimer timer;

    std::vector<std::uint32_t> scratch;
};
} // namespace

std::unique_ptr<CommandBufferBackend> makeGLCommandBuffer(Device& device)
{
    return std::make_unique<GLCommandBufferBackend>(device);
}
} // namespace eacp::GPU
