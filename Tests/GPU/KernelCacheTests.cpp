#include "Common.h"

#include <eacp/GPU/Codegen/KernelCache.h>

#include <stdexcept>
#include <string>

// sharedKernel's instances let go of their buffers after every dispatch, so a
// caller that does not assign one gets an exception naming it rather than a
// binding an earlier caller left behind.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct SharedScaleKernel final : ComputeProgram
{
    SharedScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;

    EACP_SHADER(input, output, scale)
};

// A type of its own, so that no other test's instance of it is in the cache.
struct OwnedScaleKernel final : ComputeProgram
{
    OwnedScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;

    EACP_SHADER(input, output, scale)
};

constexpr auto count = 16;

Buffer makeRamp()
{
    auto values = Vector<float> {};
    values.resize(count);

    for (auto i = 0; i < count; ++i)
        values[i] = (float) i;

    return Buffer {Device::shared(),
                   values.data(),
                   (int) sizeof(float) * count,
                   BufferUsage::Storage};
}

Buffer makeOutput()
{
    return Device::shared().makeBuffer((int) sizeof(float) * count);
}

bool holds(const Buffer& buffer, float scale)
{
    auto values = Vector<float> {};
    values.resize(count);
    buffer.read(values.data(), (int) sizeof(float) * count);

    for (auto i = 0; i < count; ++i)
        if (values[i] != (float) i * scale)
            return false;

    return true;
}

template <typename Kernel>
void dispatchOnce(Kernel& kernel)
{
    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();
}

template <typename Kernel>
std::string unassignedMemberError(Kernel& kernel)
{
    try
    {
        dispatchOnce(kernel);
    }
    catch (const std::logic_error& error)
    {
        return error.what();
    }

    return {};
}

bool mentions(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}
} // namespace

auto tSharedKernelDispatchesWhenEveryBindingIsSet =
    test("GPU/sharedKernelDispatchesWhenEveryBindingIsSet") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp();
    auto first = makeOutput();
    auto second = makeOutput();

    auto& kernel = sharedKernel<SharedScaleKernel>(device);
    kernel.input = input;
    kernel.output = first;
    kernel.scale = 2.f;
    dispatchOnce(kernel);

    kernel.input = input;
    kernel.output = second;
    kernel.scale = 3.f;
    dispatchOnce(kernel);

    check(holds(first, 2.f));
    check(holds(second, 3.f));
};

auto tSharedKernelReleasesItsBindingsAfterADispatch =
    test("GPU/sharedKernelReleasesItsBindingsAfterADispatch") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp();
    auto output = makeOutput();

    auto& kernel = sharedKernel<SharedScaleKernel>(device);
    kernel.input = input;
    kernel.output = output;
    kernel.scale = 1.f;
    dispatchOnce(kernel);

    check(kernel.input.value.buffer == nullptr);
    check(kernel.output.value.buffer == nullptr);
};

// The second caller forgets the input. Without the release the dispatch would
// read through the first caller's range, into a buffer freed by then.
auto tSharedKernelRefusesABindingLeftFromTheLastDispatch =
    test("GPU/sharedKernelRefusesABindingLeftFromTheLastDispatch") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();
    auto& kernel = sharedKernel<SharedScaleKernel>(device);

    {
        auto input = makeRamp();
        kernel.input = input;
        kernel.output = output;
        kernel.scale = 2.f;
        dispatchOnce(kernel);
    }

    kernel.output = output;
    kernel.scale = 2.f;

    auto error = unassignedMemberError(kernel);

    check(mentions(error, "SharedScaleKernel"));
    check(mentions(error, "'input'"));
    check(kernel.output.value.buffer == nullptr);
};

// A kernel its owner holds keeps what was assigned to it, which is how a
// kernel dispatched every frame over the same buffers is written.
auto tOwnedKernelKeepsItsBindings = test("GPU/ownedKernelKeepsItsBindings") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp();
    auto output = makeOutput();

    auto kernel = OwnedScaleKernel {};
    kernel.prepare(device);
    kernel.input = input;
    kernel.output = output;
    kernel.scale = 2.f;
    dispatchOnce(kernel);

    kernel.scale = 5.f;
    dispatchOnce(kernel);

    check(holds(output, 5.f));
};

auto tOwnedKernelRefusesAnUnassignedBinding =
    test("GPU/ownedKernelRefusesAnUnassignedBinding") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp();

    auto kernel = OwnedScaleKernel {};
    kernel.prepare(device);
    kernel.input = input;
    kernel.scale = 2.f;

    auto error = unassignedMemberError(kernel);

    check(mentions(error, "OwnedScaleKernel"));
    check(mentions(error, "'output'"));
};
