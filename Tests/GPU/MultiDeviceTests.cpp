#include "Common.h"

#include <thread>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Device is documented as an instantiable class whose shared() is a
// convenience, and these pin that it actually is one: two Devices are two
// queues, two command-list pools and two constant rings, and a Device built on
// a worker thread runs work there without touching the main thread's.
//
// That is what lets an inference worker submit at 30 fps without queueing
// behind the UI's frames. Metal has always honoured it - Device::Native holds
// its MTLCommandQueue as an instance member - and it is the D3D12 backend these
// caught, where every Device forwarded to one process-wide context.

namespace
{
struct ScaleKernel final : ComputeProgram
{
    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;
    EACP_SHADER(input, output, scale)

    ScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }
};

constexpr int count = 256;

// Runs the kernel end to end on whichever Device it is handed, and answers
// whether every element came back scaled. The whole body - buffers, pipeline,
// command buffer, read-back - belongs to that one Device, which is the
// contract: resources do not cross Devices.
bool scaleRunsOn(Device& device, float scale)
{
    float input[count] = {};

    for (auto i = 0; i < count; ++i)
        input[i] = static_cast<float>(i);

    auto inputBuffer = device.makeBuffer(input, BufferUsage::Storage);
    auto outputBuffer = device.makeBuffer(sizeof(input));

    auto kernel = ScaleKernel {};
    kernel.input = inputBuffer;
    kernel.output = outputBuffer;
    kernel.scale = scale;
    kernel.prepare(device);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    float result[count] = {};
    outputBuffer.read(result, sizeof(result));

    for (auto i = 0; i < count; ++i)
        if (result[i] != input[i] * scale)
            return false;

    return true;
}
} // namespace

// The one assertion the whole refactor turns on: a second Device is a second
// queue. Where it is not, every submission in the process is strictly FIFO
// behind every other, whatever thread made it.
auto tDevicesHaveTheirOwnQueue = test("GPU/devicesHaveTheirOwnQueue") = []
{
    if (!Device::shared().isValid())
        return;

    auto worker = Device();
    check(worker.isValid());

    check(worker.nativeQueue() != nullptr);
    check(worker.nativeQueue() != Device::shared().nativeQueue());

    // The underlying GPU is deliberately the same one: an MTLBuffer belongs to
    // its MTLDevice and a D3D12 resource to its ID3D12Device, so two Devices
    // sharing the adapter is what makes a queue per Device cheap.
    check(worker.nativeDevice() == Device::shared().nativeDevice());
};

// Both Devices run the same kernel with different scales and each reads back
// its own answer. A shared command-list pool or constant ring shows up here as
// one Device's uniforms arriving in the other's dispatch.
auto tTwoDevicesRunIndependently = test("GPU/twoDevicesRunIndependently") = []
{
    if (!Device::shared().isValid())
        return;

    auto worker = Device();

    if (!worker.isValid())
        return;

    check(scaleRunsOn(Device::shared(), 2.f));
    check(scaleRunsOn(worker, 7.f));
    check(scaleRunsOn(Device::shared(), 3.f));
};

// A worker Device may touch Device::shared() — compiling a kernel through the
// no-argument prepare() does — without taking ownership of it. The process-wide
// Device follows the main thread, so the next frame the UI draws is still legal.
auto tSharedDeviceStaysWithTheMainThread =
    test("GPU/sharedDeviceStaysWithTheMainThread") = []
{
    if (!Device::shared().isValid())
        return;

    auto reachedFromWorker = false;

    auto worker =
        std::thread([&] { reachedFromWorker = Device::shared().isValid(); });
    worker.join();

    check(reachedFromWorker);

    // The assertion inside acquire() is what would fire if the worker above had
    // taken the shared Device with it.
    check(scaleRunsOn(Device::shared(), 2.f));
};

// The point of all of it: a Device constructed on, owned by and used from a
// thread that is not the main one. Everything it touches - its queue, its
// pools, its buffers - was made on that thread and dies there.
auto tDeviceRunsOnAWorkerThread = test("GPU/deviceRunsOnAWorkerThread") = []
{
    if (!Device::shared().isValid())
        return;

    auto ran = false;
    auto valid = false;

    auto worker = std::thread(
        [&]
        {
            auto device = Device();
            valid = device.isValid();

            if (valid)
                ran = scaleRunsOn(device, 5.f);
        });

    worker.join();

    check(valid);
    check(ran);
};

// Two worker threads at once, each with its own Device, is the shape the
// gesture pipeline actually wants: a camera worker submitting inference while
// the main thread renders. Neither wait blocks on the other's fence.
//
// Repeated rather than run once, because what this is watching for is a race:
// a command-list pool or constant ring shared between the two hands one
// thread's recording to the other, and a single pass each is far too short a
// window to land in.
auto tWorkerDevicesRunConcurrently = test("GPU/workerDevicesRunConcurrently") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto runs = 20;

    auto results = Array<bool, 2> {};

    auto runOne = [&results](int index, float scale)
    {
        auto device = Device();
        results[index] = device.isValid();

        for (auto run = 0; run < runs && results[index]; ++run)
            results[index] = scaleRunsOn(device, scale);
    };

    auto first = std::thread(runOne, 0, 4.f);
    auto second = std::thread(runOne, 1, 6.f);

    first.join();
    second.join();

    check(results[0]);
    check(results[1]);
};
