#include "Common.h"

#include <eacp/GPU/Buffer/BufferPool.h>

#include <cstdint>

// BufferPool - Device::makeBuffer(bytes) recycling the storage of buffers the
// GPU has finished with, and never the storage of one it may still be using.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto count = 1024;

// A size of its own per test, so that one test's storage left in the pool is
// never what another is looking for.
constexpr std::int64_t sizeFor(int test)
{
    return (std::int64_t) sizeof(float) * (count + 16 * test);
}

struct RampKernel final : ComputeProgram
{
    RampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

void writeRampInto(Device& device, CommandBuffer& commands, const Buffer& buffer)
{
    auto kernel = RampKernel {};
    kernel.output = buffer;
    kernel.prepare(device);

    auto pass = commands.beginCompute();
    pass.dispatch(kernel, count);
}

void submitSomethingElse(Device& device)
{
    auto scratch = device.makeBuffer(4);
    auto commands = device.makeCommandBuffer();
    commands.fill(scratch);
    commands.commit();
}

void* storageOf(const Buffer& buffer)
{
    return buffer.nativeBuffer();
}
} // namespace

auto tSubmissionsAreCounted = test("GPU/bufferPoolSubmissionsAreCounted") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto before = device.lastSubmission();

    auto buffer = device.makeBuffer(sizeFor(1));
    auto commands = device.makeCommandBuffer();
    writeRampInto(device, commands, buffer);
    commands.commit();

    check(device.lastSubmission() == before + 1);
    check(device.hasFinished(device.lastSubmission()));
    check(!device.hasFinished(device.lastSubmission() + 1));
};

// Made, used, finished with, destroyed: the next buffer of that size is the
// same storage, and no new one was created for it.
auto tFinishedStorageIsReused = test("GPU/bufferPoolReusesFinishedStorage") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    void* first = nullptr;

    {
        auto buffer = device.makeBuffer(sizeFor(2));
        first = storageOf(buffer);

        auto commands = device.makeCommandBuffer();
        writeRampInto(device, commands, buffer);
        commands.commit();
    }

    // Kept until the submission after it finishes - the one that would have
    // been recording when the buffer went.
    submitSomethingElse(device);

    auto created = device.buffersCreated();
    auto second = device.makeBuffer(sizeFor(2));

    check(storageOf(second) == first);
    check(device.buffersCreated() == created);
};

// Destroyed while the command buffer that writes it is still being recorded:
// a buffer made in the same recording must not be handed that storage, or the
// two dispatches would write one place.
auto tStorageInTheOpenRecordingIsNotReused =
    test("GPU/bufferPoolKeepsStorageOfTheOpenRecording") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto commands = device.makeCommandBuffer();
    void* first = nullptr;

    {
        auto buffer = device.makeBuffer(sizeFor(3));
        first = storageOf(buffer);
        writeRampInto(device, commands, buffer);
    }

    auto second = device.makeBuffer(sizeFor(3));
    check(storageOf(second) != first);

    writeRampInto(device, commands, second);
    commands.commit();
};

// Destroyed while its command buffer may still be running: not handed out
// again until the GPU says it has finished.
auto tStorageInFlightIsNotReused = test("GPU/bufferPoolKeepsStorageInFlight") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto commands = device.makeCommandBuffer();
    void* first = nullptr;

    {
        auto buffer = device.makeBuffer(sizeFor(4));
        first = storageOf(buffer);
        writeRampInto(device, commands, buffer);
        commands.submit();
    }

    auto stillRunning = !device.hasFinished(device.lastSubmission());
    auto second = device.makeBuffer(sizeFor(4));

    if (stillRunning)
        check(storageOf(second) != first);

    commands.wait();
};

// Storage nothing asks for again is let go after a few submissions rather than
// held: asking for that size afterwards creates a buffer.
auto tUnusedStorageIsFreed = test("GPU/bufferPoolFreesUnusedStorage") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    {
        auto buffer = device.makeBuffer(sizeFor(5));
        auto commands = device.makeCommandBuffer();
        writeRampInto(device, commands, buffer);
        commands.commit();
    }

    for (auto i = std::uint64_t {0}; i < BufferPool::submissionsKeptUnused + 2; ++i)
        submitSomethingElse(device);

    auto created = device.buffersCreated();
    auto again = device.makeBuffer(sizeFor(5));

    check(again.isValid());
    check(device.buffersCreated() == created + 1);
};
