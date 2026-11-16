#include "Common.h"

#include <cstdint>

// Storage-buffer slots past the portable eight: a hand-written kernel reading
// slot 12 and writing slot 15, on the backends whose Device says it has them.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace ComputeBufferSlotTests
{
namespace
{
constexpr auto count = 100;

static_assert(webgpuBufferBinding(7) == 8);
static_assert(webgpuBufferBinding(12) == 13);
static_assert(webgpuBufferBinding(15) == 16);
static_assert(vulkanComputeBufferBinding(7) == 7);
static_assert(vulkanComputeBufferBinding(12) == 21);
static_assert(vulkanComputeBufferBinding(15) == 24);
static_assert(vulkanComputeUniformBinding == 16);

struct Params
{
    float scale;
    std::uint32_t count;
};

bool hasHighSlots()
{
    auto& device = Device::shared();
    return device.isValid() && device.maxComputeBufferSlots() > 15;
}

Buffer makeFilled(float first, float step)
{
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add(first + step * (float) i);

    return Buffer {Device::shared(),
                   values.data(),
                   (std::int64_t) sizeof(float) * count,
                   BufferUsage::Storage};
}

ComputePipeline makeHighSlotPipeline()
{
    auto source = nativeComputeShaderSource("ComputeBufferSlot/highSlots")
                      .withCompute("computeMain");
    auto library = Device::shared().makeShaderLibrary(source);

    return Device::shared().makeComputePipeline(library);
}

Vector<float> runHighSlotKernel(bool bindPastTheEnd)
{
    auto pipeline = makeHighSlotPipeline();
    auto low = makeFilled(1.f, 1.f);
    auto high = makeFilled(100.f, 10.f);
    auto result = makeFilled(-1.f, 0.f);
    auto decoy = makeFilled(0.f, 0.f);

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(pipeline);
        pass.setInputBuffer(low, 7);
        pass.setInputBuffer(high, 12);
        pass.setOutputBuffer(result, 15);
        pass.setUniform(Params {2.f, (std::uint32_t) count});

        if (bindPastTheEnd)
            pass.setInputBuffer(decoy, ComputePass::maxBufferSlots);

        pass.dispatch(count);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(count);
    result.read(values.data(), (std::int64_t) sizeof(float) * count);

    return values;
}

bool matchesExpected(const Vector<float>& values)
{
    for (auto i = 0; i < count; ++i)
    {
        auto expected = (1.f + (float) i) + (100.f + 10.f * (float) i) * 2.f;

        if (values[i] != expected)
            return false;
    }

    return true;
}
} // namespace

auto tDeviceReportsSlots = test("ComputeBufferSlots/deviceReportsItsLimit") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto slots = device.maxComputeBufferSlots();

    check(slots >= ComputePass::portableBufferSlots);
    check(slots <= ComputePass::maxBufferSlots);

    if constexpr (Platform::isApple())
        check(slots == ComputePass::maxBufferSlots);

    if constexpr (Platform::isWindows())
        check(slots == ComputePass::portableBufferSlots);
};

auto tReadsSlotTwelve = test("ComputeBufferSlots/kernelReadsSlotTwelve") = []
{
    if (!hasHighSlots())
        return;

    check(matchesExpected(runHighSlotKernel(false)));
};

auto tPastTheEndDropped =
    test("ComputeBufferSlots/bindPastTheLastSlotIsDropped") = []
{
    if (!hasHighSlots())
        return;

    check(matchesExpected(runHighSlotKernel(true)));
};
} // namespace ComputeBufferSlotTests
