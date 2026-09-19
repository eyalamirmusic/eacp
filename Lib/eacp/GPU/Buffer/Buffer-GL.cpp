#include "Buffer.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
// Every create, update and read goes through GL_COPY_WRITE_BUFFER, which no
// draw reads: binding a buffer to its usage target to fill it would knock the
// vertex or index binding a pass had set out from under it.
constexpr GLenum glScratchTarget = GL_COPY_WRITE_BUFFER;

struct GLBufferBackend final : BufferBackend
{
    GLBufferBackend(Device& device,
                    const void* data,
                    std::int64_t byteCount,
                    BufferUsage usage,
                    BufferStorage storage)
        : context(getGLContext(device))
        , owner(&device)
    {
        const auto bytes = byteCount > 0 ? byteCount : 0;

        bufferData.size = bytes;

        if (!context.isValid() || bytes == 0)
            return;

        context.makeCurrent();

        glGenBuffers(1, &bufferData.buffer);

        if (bufferData.buffer == 0)
            return;

        glBindBuffer(glScratchTarget, bufferData.buffer);

        // A Storage buffer keeps device storage whatever was asked for: a
        // kernel writing it is what a persistent host mapping cannot carry.
        const auto streaming =
            storage == BufferStorage::Streaming && usage != BufferUsage::Storage;

        if (!streaming || !mapStreamingStorage(data, bytes))
            glBufferData(glScratchTarget,
                         (GLsizeiptr) bytes,
                         data,
                         streaming ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);

        glBindBuffer(glScratchTarget, 0);
        glDrainErrors("buffer creation");
    }

    ~GLBufferBackend() override
    {
        if (bufferData.buffer == 0)
            return;

        context.makeCurrent();

        if (bufferData.mapped != nullptr)
        {
            glBindBuffer(glScratchTarget, bufferData.buffer);
            glUnmapBuffer(glScratchTarget);
            glBindBuffer(glScratchTarget, 0);
        }

        glDeleteBuffers(1, &bufferData.buffer);
    }

    // False leaves the caller to fall through to the ordinary glBufferData.
    bool mapStreamingStorage(const void* data, std::int64_t bytes)
    {
        const auto& caps = context.getCapabilities();

        if (!caps.bufferStorage)
            return false;

        constexpr GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT
                                     | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT;

        if (caps.bufferStorageIsEXT)
            glBufferStorageEXT(glScratchTarget, (GLsizeiptr) bytes, data, flags);
        else
            glBufferStorage(glScratchTarget, (GLsizeiptr) bytes, data, flags);

        // A driver that refused the allocation leaves nothing to map, and the
        // error it raised must not reach the next drain as this one's.
        if (glGetError() != GL_NO_ERROR)
            return false;

        bufferData.mapped = static_cast<std::byte*>(
            glMapBufferRange(glScratchTarget,
                             0,
                             (GLsizeiptr) bytes,
                             GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT
                                 | GL_MAP_COHERENT_BIT));

        if (bufferData.mapped != nullptr)
            return true;

        // The storage is immutable and cannot be re-specified, so the buffer
        // is thrown away and made again by the caller's glBufferData.
        glBindBuffer(glScratchTarget, 0);
        glDeleteBuffers(1, &bufferData.buffer);
        glGenBuffers(1, &bufferData.buffer);
        glBindBuffer(glScratchTarget, bufferData.buffer);

        return false;
    }

    // Clamped to the buffer's end, and the count that survives it, or zero
    // where there is nothing to move.
    std::int64_t clamp(std::int64_t byteCount, std::int64_t byteOffset) const
    {
        if (bufferData.buffer == 0 || byteCount <= 0 || byteOffset < 0
            || byteOffset >= bufferData.size)
            return 0;

        const auto available = bufferData.size - byteOffset;

        return byteCount < available ? byteCount : available;
    }

    void write(const void* data, std::int64_t byteCount, std::int64_t byteOffset)
    {
        const auto count = data != nullptr ? clamp(byteCount, byteOffset) : 0;

        if (count == 0)
            return;

        context.makeCurrent();

        if (bufferData.mapped != nullptr)
        {
            std::memcpy(bufferData.mapped + byteOffset, data, (std::size_t) count);
            return;
        }

        glBindBuffer(glScratchTarget, bufferData.buffer);
        glBufferSubData(
            glScratchTarget, (GLintptr) byteOffset, (GLsizeiptr) count, data);
        glBindBuffer(glScratchTarget, 0);

        glDrainErrors("buffer update");
    }

    void assertOwningThread() const
    {
        if (owner != nullptr)
            owner->assertOwningThread();
    }

    std::int64_t size() const override { return bufferData.size; }

    bool isValid() const override { return bufferData.isValid(); }

    void read(void* dst,
              std::int64_t byteCount,
              std::int64_t byteOffset) const override
    {
        assertOwningThread();

        const auto count = dst != nullptr ? clamp(byteCount, byteOffset) : 0;

        if (count == 0)
            return;

        context.makeCurrent();

        // Nothing on the GPU writes host storage, so there is nothing to wait
        // for - the Streaming exception Buffer::read states.
        if (bufferData.mapped != nullptr)
        {
            std::memcpy(dst, bufferData.mapped + byteOffset, (std::size_t) count);
            return;
        }

        glBindBuffer(glScratchTarget, bufferData.buffer);

        // Both of these order themselves against everything recorded before
        // them, which is the "valid once committed" rule the header states.
        if (context.getCapabilities().getBufferSubData)
        {
            glGetBufferSubData(
                glScratchTarget, (GLintptr) byteOffset, (GLsizeiptr) count, dst);
        }
        else if (auto* source = glMapBufferRange(glScratchTarget,
                                                 (GLintptr) byteOffset,
                                                 (GLsizeiptr) count,
                                                 GL_MAP_READ_BIT);
                 source != nullptr)
        {
            std::memcpy(dst, source, (std::size_t) count);
            glUnmapBuffer(glScratchTarget);
        }

        glBindBuffer(glScratchTarget, 0);
        glDrainErrors("buffer read");
    }

    void update(const void* data,
                std::int64_t byteCount,
                std::int64_t byteOffset) override
    {
        assertOwningThread();

        // Only the host-mapped shape needs the wait. A glBufferSubData below is
        // recorded into the context's own stream, and the stream is already the
        // ordering - see the rule on Buffer::update.
        if (bufferData.mapped != nullptr && context.isValid())
        {
            context.makeCurrent();
            glFinish();
        }

        write(data, byteCount, byteOffset);
    }

    void updateUnordered(const void* data,
                         std::int64_t byteCount,
                         std::int64_t byteOffset) override
    {
        assertOwningThread();

        write(data, byteCount, byteOffset);
    }

    void* nativeBuffer() const override { return &bufferData; }

    // One buffer name whichever way a kernel uses it.
    void* nativeReadView() const override { return &bufferData; }

    void* nativeWriteView() const override { return &bufferData; }

    // A buffer never moves between Devices.
    GLContext& context;

    // The Device beside it, for the thread rule alone.
    Device* owner = nullptr;

    mutable GLBufferData bufferData;
};
} // namespace

std::unique_ptr<BufferBackend> makeGLBuffer(Device& device,
                                            const void* data,
                                            std::int64_t bytes,
                                            BufferUsage usage,
                                            BufferStorage storage)
{
    return std::make_unique<GLBufferBackend>(device, data, bytes, usage, storage);
}
} // namespace eacp::GPU
