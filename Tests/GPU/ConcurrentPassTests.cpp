#include "Common.h"

// A pass whose dispatches may overlap, and the barrier that orders the ones
// that may not.
//
// Nothing here can assert on the overlap itself - a concurrent pass is a
// permission, not a promise, and a device is free to run the dispatches one
// after another anyway. What is checkable is that the answer is the same: a
// chain with a barrier between its stages, independent dispatches with none, a
// serial pass whose barriers are no-ops, and a second pass reading what a
// concurrent one left.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto floatBytes = (int) sizeof(float);
constexpr auto elementCount = 256;

constexpr auto sliceElements = 64;
constexpr auto sliceCount = 32;

// output[i] = base + i, so which dispatch wrote a slice is visible in it.
struct RampKernel final : ComputeProgram
{
    RampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, base + toFloat(i));
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> base;

    EACP_SHADER(output, base)
};

// One link of a chain: whatever the stage before it wrote, scaled and shifted.
struct ScaleAddKernel final : ComputeProgram
{
    ScaleAddKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale + bias);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;
    Uniform<Float> bias;

    EACP_SHADER(input, output, scale, bias)
};

// The grid a ranged storage bind's offset has to sit on. Every slice below
// starts on it, so each one is bindable on every backend.
int sliceStrideBytes()
{
    const auto alignment = Device::shared().storageBufferOffsetAlignment();
    const auto wanted = sliceElements * floatBytes;

    return ((wanted + alignment - 1) / alignment) * alignment;
}

int sliceStrideElements()
{
    return sliceStrideBytes() / floatBytes;
}

BufferRange slice(const Buffer& buffer, int index)
{
    return {&buffer, index * sliceStrideBytes(), sliceElements * floatBytes};
}

float sliceBase(int index)
{
    return (float) (index * 1000);
}

Buffer makeFilled(int elements, float value)
{
    auto values = Vector<float> {};
    values.assign(elements, value);

    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * elements,
                   BufferUsage::Storage};
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), floatBytes * elements);
    return values;
}

// A ramp, doubled, then offset by one: 2i + 1 out of three dispatches that only
// give that answer if each saw the one before it.
struct Chain
{
    Chain()
        : stage(makeFilled(elementCount, -1.f))
        , doubled(makeFilled(elementCount, -1.f))
        , result(makeFilled(elementCount, -1.f))
    {
        ramp.output = stage;
        ramp.base = 0.f;
        ramp.prepare();

        doubleStage.input = stage;
        doubleStage.output = doubled;
        doubleStage.scale = 2.f;
        doubleStage.bias = 0.f;
        doubleStage.prepare();

        offsetStage.input = doubled;
        offsetStage.output = result;
        offsetStage.scale = 1.f;
        offsetStage.bias = 1.f;
        offsetStage.prepare();
    }

    void recordInto(ComputePass& pass)
    {
        pass.dispatch(ramp, elementCount);
        pass.barrier();
        pass.dispatch(doubleStage, elementCount);
        pass.barrier();
        pass.dispatch(offsetStage, elementCount);
    }

    void checkResult() const
    {
        auto values = readFloats(result, elementCount);

        for (auto i = 0; i < elementCount; ++i)
            check(values[i] == 2.f * (float) i + 1.f);
    }

    Buffer stage;
    Buffer doubled;
    Buffer result;

    RampKernel ramp;
    ScaleAddKernel doubleStage;
    ScaleAddKernel offsetStage;
};

// A frame's own concurrent pass, checked through the buffer it left behind
// rather than through a pixel - the render pass after it is there only to make
// the frame an ordinary one.
struct ConcurrentFrameView final : GPUView
{
    ConcurrentFrameView() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute({}, DispatchOrder::Concurrent);
            chain.recordInto(compute);
        }

        frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
    }

    Chain chain;
};
} // namespace

// The chain the barriers exist for: three dependent stages in one concurrent
// pass, each reading what the one before it wrote.
auto tBarrierOrdersDependentStages =
    test("ConcurrentPass/barrierOrdersDependentStages") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto chain = Chain {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute({}, DispatchOrder::Concurrent);
        chain.recordInto(pass);
    }

    commands.commit();
    chain.checkResult();
};

// The case the mode is for: dispatches that depend on nothing, recorded with no
// barrier between them, all landing where they were told to.
auto tIndependentDispatchesAllLand =
    test("ConcurrentPass/independentDispatchesAllLand") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto stride = sliceStrideElements();
    auto output = makeFilled(stride * sliceCount, -1.f);

    auto kernel = RampKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute({}, DispatchOrder::Concurrent);

        for (auto index = 0; index < sliceCount; ++index)
        {
            kernel.output = slice(output, index);
            kernel.base = sliceBase(index);
            pass.dispatch(kernel, sliceElements);
        }
    }

    commands.commit();

    auto values = readFloats(output, stride * sliceCount);

    for (auto index = 0; index < sliceCount; ++index)
        for (auto i = 0; i < sliceElements; ++i)
            check(values[index * stride + i] == sliceBase(index) + (float) i);
};

// The same chain in a serial pass, where every barrier() is a no-op and the
// dispatches were ordered without it.
auto tSerialPassIgnoresBarriers =
    test("ConcurrentPass/serialPassIgnoresBarriers") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto chain = Chain {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        chain.recordInto(pass);
    }

    commands.commit();
    chain.checkResult();
};

// What the per-dispatch barriers used to owe the rest of the recording: a pass
// after a concurrent one reads everything it wrote.
auto tNextPassSeesTheConcurrentPass =
    test("ConcurrentPass/aLaterPassSeesWhatAConcurrentOneWrote") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto stride = sliceStrideElements();
    const auto capacity = stride * sliceCount;

    auto filled = makeFilled(capacity, -1.f);
    auto doubled = makeFilled(capacity, -1.f);

    auto ramp = RampKernel {};
    ramp.prepare();

    auto scale = ScaleAddKernel {};
    scale.input = filled;
    scale.output = doubled;
    scale.scale = 2.f;
    scale.bias = 0.f;
    scale.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute({}, DispatchOrder::Concurrent);

        for (auto index = 0; index < sliceCount; ++index)
        {
            ramp.output = slice(filled, index);
            ramp.base = sliceBase(index);
            pass.dispatch(ramp, sliceElements);
        }
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(scale, capacity);
    }

    commands.commit();

    auto values = readFloats(doubled, capacity);

    for (auto index = 0; index < sliceCount; ++index)
        for (auto i = 0; i < sliceElements; ++i)
            check(values[index * stride + i]
                  == 2.f * (sliceBase(index) + (float) i));
};

// And the same on a frame's own command buffer, which begins its passes through
// a path of its own.
auto tConcurrentPassOnTheFrame =
    test("ConcurrentPass/aFrameBeginsAConcurrentPassToo") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ConcurrentFrameView {};
    view.setBounds({0.f, 0.f, 8.f, 4.f});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    view.chain.checkResult();
};
