#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"

namespace eacp::GPU
{
namespace
{
// GL_TIMESTAMP queries in the slot layout the Vulkan query pool uses: two per
// timed pass, then the frame's own pair. A query reports nanoseconds, so there
// is no period to scale by.
struct GLGpuTimestamps final : GpuTimestampsBackend
{
    // Deferred: this is built by Device, which is not yet itself when that runs.
    void ensureCreated(Device& owner)
    {
        if (tried)
            return;

        tried = true;

        if (!owner.isValid())
            return;

        context = &getGLContext(owner);

        const auto& caps = context->getCapabilities();

        if (!caps.timerQuery)
            return;

        context->makeCurrent();

        for (auto& slot: slots)
        {
            slot.useEXT = caps.timerQueryIsEXT;

            if (caps.timerQueryIsEXT)
                glGenQueriesEXT(GLTimestampSlot::queryCount, slot.queries.data());
            else
                glGenQueries(GLTimestampSlot::queryCount, slot.queries.data());

            slot.created = slot.queries[0] != 0;

            if (!slot.created)
                return;
        }

        supported = true;
    }

    ~GLGpuTimestamps() override
    {
        if (context == nullptr || !context->isValid())
            return;

        context->makeCurrent();

        for (auto& slot: slots)
        {
            if (!slot.created)
                continue;

            if (slot.useEXT)
                glDeleteQueriesEXT(GLTimestampSlot::queryCount, slot.queries.data());
            else
                glDeleteQueries(GLTimestampSlot::queryCount, slot.queries.data());
        }
    }

    // GL_QUERY_RESULT is not available until the query has been written *and*
    // retired, so an unwritten one would block the read of the whole slot
    // forever. Every slot therefore records which indices it wrote.
    std::uint64_t readQuery(const GLTimestampSlot& slot, int index) const
    {
        auto available = GLuint {0};

        if (slot.useEXT)
            glGetQueryObjectuivEXT(
                slot.queries[index], GL_QUERY_RESULT_AVAILABLE, &available);
        else
            glGetQueryObjectuiv(
                slot.queries[index], GL_QUERY_RESULT_AVAILABLE, &available);

        if (available == 0)
            return 0;

        auto value = GLuint64 {0};

        if (slot.useEXT)
            glGetQueryObjectui64vEXT(slot.queries[index], GL_QUERY_RESULT, &value);
        else
            glGetQueryObjectui64v(slot.queries[index], GL_QUERY_RESULT, &value);

        return value;
    }

    bool isSupported() const override { return supported; }

    void beginSlot(int slot, Device& device) override
    {
        ensureCreated(device);

        if (!supported)
            return;

        submitted[slot] = false;
    }

    void beginRecording(int slot, void*) override
    {
        if (!supported)
            return;

        context->makeCurrent();
        slots[slot].writeTimestamp(GLTimestampSlot::frameStartQuery);
    }

    void* nativeSamples(int slot) const override
    {
        if (!supported)
            return nullptr;

        return const_cast<GLTimestampSlot*>(&slots[slot]);
    }

    bool endSlot(int slot, int, void*) override
    {
        if (!supported)
            return false;

        context->makeCurrent();
        slots[slot].writeTimestamp(GLTimestampSlot::frameEndQuery);

        // A query the driver has taken retires with the commands ahead of it,
        // and nothing else has to be recorded for the read below.
        glFlush();

        return true;
    }

    void noteSubmitted(int slot, std::uint64_t) override
    {
        if (!supported)
            return;

        submitted[slot] = true;
    }

    bool isSlotComplete(int slot, const Device&) const override
    {
        if (!submitted[slot])
            return false;

        context->makeCurrent();

        auto available = GLuint {0};
        const auto& entry = slots[slot];

        if (entry.useEXT)
            glGetQueryObjectuivEXT(entry.queries[GLTimestampSlot::frameEndQuery],
                                   GL_QUERY_RESULT_AVAILABLE,
                                   &available);
        else
            glGetQueryObjectuiv(entry.queries[GLTimestampSlot::frameEndQuery],
                                GL_QUERY_RESULT_AVAILABLE,
                                &available);

        return available != 0;
    }

    double resolveSlot(int slot, int passCount, double* milliseconds) override
    {
        if (!supported)
            return 0.0;

        context->makeCurrent();

        // The ES form is allowed to throw a frame's timings away when the GPU
        // was reset under it, and says so once for the whole frame.
        if (slots[slot].useEXT)
        {
            auto disjoint = GLint {0};
            glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);

            if (disjoint != 0)
            {
                for (auto pass = 0; pass < passCount; ++pass)
                    milliseconds[pass] = 0.0;

                return 0.0;
            }
        }

        const auto& entry = slots[slot];

        const auto toMilliseconds = [](std::uint64_t start, std::uint64_t end)
        {
            // An unwritten query reads as zero, a disjoint one backwards.
            if (end <= start)
                return 0.0;

            return static_cast<double>(end - start) / 1'000'000.0;
        };

        for (auto pass = 0; pass < passCount; ++pass)
            milliseconds[pass] = toMilliseconds(readQuery(entry, pass * 2),
                                                readQuery(entry, pass * 2 + 1));

        const auto frameStart = readQuery(entry, GLTimestampSlot::frameStartQuery);
        const auto frameEnd = readQuery(entry, GLTimestampSlot::frameEndQuery);

        // Absolute nanoseconds, read once the frame completed, so both being
        // zero cannot mean a quick frame - only that the driver never wrote
        // them, which is a timer this device does not actually have.
        if (frameStart == 0 && frameEnd == 0)
            supported = false;

        return toMilliseconds(frameStart, frameEnd);
    }

    GLContext* context = nullptr;

    Array<GLTimestampSlot, GpuTimestamps::slotCount> slots;
    Array<bool, GpuTimestamps::slotCount> submitted {};

    bool supported = false;
    bool tried = false;
};
} // namespace

// Out of line rather than in GLTypes.h, so that header stays the struct
// definitions the -GL.cpp files cast to and nothing else.
void GLTimestampSlot::writeTimestamp(int index) const
{
    if (!created)
        return;

    if (useEXT)
        glQueryCounterEXT(queries[index], GL_TIMESTAMP_EXT);
    else
        glQueryCounter(queries[index], GL_TIMESTAMP);
}

std::unique_ptr<GpuTimestampsBackend> makeGLGpuTimestamps()
{
    return std::make_unique<GLGpuTimestamps>();
}
} // namespace eacp::GPU
