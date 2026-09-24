#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/TensorOps.h>

#include <cmath>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
void checkClose(const std::vector<float>& actual,
                const std::vector<float>& expected,
                float tolerance)
{
    check(actual.size() == expected.size());

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        check(std::abs(actual[i] - expected[i]) <= tolerance);
}

Tensor tensorOf(std::vector<float> values, std::vector<int> shape)
{
    return Tensor::fromHostF32(values.data(), std::move(shape));
}
} // namespace

auto tAddTensors = test("TensorOps/addMatchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto a =
        Tensor::fromHostF32(std::vector<float> {1.f, 2.f, 3.f}.data(), {3}, device);
    auto b = Tensor::fromHostF32(
        std::vector<float> {10.f, 20.f, 30.f}.data(), {3}, device);

    auto commands = device.makeCommandBuffer();
    auto result = Tensor::uninitializedF32({3}, device);

    {
        auto pass = commands.beginCompute();
        result = add(pass, a, b, device);
    }

    commands.commit();

    checkClose(result.toHostF32(), {11.f, 22.f, 33.f}, 1.0e-5f);
};

auto tConcatAndSliceRows = test("TensorOps/concatRowsThenSliceRowsRoundTrips") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto top = Tensor::fromHostF32(
        std::vector<float> {1.f, 2.f, 3.f, 4.f}.data(), {2, 2}, device);
    auto bottom =
        Tensor::fromHostF32(std::vector<float> {5.f, 6.f}.data(), {1, 2}, device);

    auto commands = device.makeCommandBuffer();
    auto sliced = Tensor::uninitializedF32({1, 2}, device);

    {
        auto pass = commands.beginCompute();
        auto joined = concatRows(pass, {top, bottom}, device);
        sliced = sliceRows(pass, joined, 2, 1, device);
    }

    commands.commit();

    checkClose(sliced.toHostF32(), {5.f, 6.f}, 1.0e-5f);
};

auto tSliceColumns = test("TensorOps/sliceColumnsMatchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x = Tensor::fromHostF32(
        std::vector<float> {1.f, 2.f, 3.f, 4.f, 5.f, 6.f}.data(), {1, 6}, device);

    auto commands = device.makeCommandBuffer();
    auto sliced = Tensor::uninitializedF32({1, 2}, device);

    {
        auto pass = commands.beginCompute();
        sliced = sliceColumns(pass, x, 2, 2, device);
    }

    commands.commit();

    checkClose(sliced.toHostF32(), {3.f, 4.f}, 1.0e-5f);
};

auto tSubtractMultiplyScaleAndAdd =
    test("TensorOps/subtractMultiplyAndScaleAndAddMatchReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto a = tensorOf({1.f, 2.f, 3.f}, {3});
    auto b = tensorOf({10.f, 20.f, 30.f}, {3});

    auto commands = device.makeCommandBuffer();
    auto difference = Tensor::uninitializedF32({3});
    auto product = Tensor::uninitializedF32({3});
    auto blend = Tensor::uninitializedF32({3});

    {
        auto pass = commands.beginCompute();
        difference = subtract(pass, a, b);
        product = multiply(pass, a, b);
        blend = scaleAndAdd(pass, a, 2.f, b, -0.5f);
    }

    commands.commit();

    checkClose(difference.toHostF32(), {-9.f, -18.f, -27.f}, 0.f);
    checkClose(product.toHostF32(), {10.f, 40.f, 90.f}, 0.f);
    checkClose(blend.toHostF32(), {-3.f, -6.f, -9.f}, 0.f);
};

auto tConcatThreeAndPad = test("TensorOps/concatRowsOfThreeThenPadWithZeros") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto head = tensorOf({1.f, 2.f}, {1, 2});
    auto body = tensorOf({3.f, 4.f, 5.f, 6.f}, {2, 2});
    auto tail = tensorOf({7.f, 8.f}, {1, 2});

    auto commands = device.makeCommandBuffer();
    auto padded = Tensor::uninitializedF32({1, 2});

    {
        auto pass = commands.beginCompute();
        auto joined = concatRows(pass, {head, body, tail});
        padded = padRowsWithZeros(pass, joined, 3);
    }

    commands.commit();

    check(padded.rows() == 6);
    checkClose(padded.toHostF32(),
               {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 0.f, 0.f, 0.f, 0.f},
               0.f);
};

auto tReshapeKeepsValues = test("TensorOps/reshapeKeepsTheValues") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto x = tensorOf({1.f, 2.f, 3.f, 4.f, 5.f, 6.f}, {2, 3, 1});
    auto flat = reshape(std::move(x), {3, 2});

    check(flat.rows() == 3);
    check(flat.cols() == 2);
    checkClose(flat.toHostF32(), {1.f, 2.f, 3.f, 4.f, 5.f, 6.f}, 0.f);
};
