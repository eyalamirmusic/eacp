#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Attention.h>
#include <eacp/ML/Kernels/BandedAttention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/TensorOps.h>

#include <cmath>
#include <memory>
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

namespace
{
Tensor scattered(int rows, int cols, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < rows * cols; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return tensorOf(values, {rows, cols});
}
} // namespace

auto tViewsReadWhatCopiesRead =
    test("TensorView/columnViewsGiveTheBitsOfColumnCopies") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 6, heads = 2, headDim = 8, dim = heads * headDim;

    auto qkv = scattered(rows, 3 * dim, 1);
    auto gamma = scattered(1, headDim, 2);
    auto beta = scattered(1, headDim, 3);
    auto band = AttentionBand {.leftRadius = 2, .rightRadius = 1, .segmentRows = 3};

    auto commands = device.makeCommandBuffer();
    auto fromViews = std::vector<Tensor> {};
    auto fromCopies = std::vector<Tensor> {};

    {
        auto pass = commands.beginCompute();

        auto q = sliceColumns(pass, qkv, 0, dim);
        auto k = sliceColumns(pass, qkv, dim, dim);
        auto v = sliceColumns(pass, qkv, 2 * dim, dim);

        fromViews.push_back(
            rmsNormPerHead(pass, qkv.columns(dim, dim), gamma, headDim, 1e-6f));
        fromCopies.push_back(rmsNormPerHead(pass, k, gamma, headDim, 1e-6f));

        fromViews.push_back(dynamicTanhPerHead(
            pass, qkv.columns(0, dim), gamma, beta, 0.5f, headDim));
        fromCopies.push_back(
            dynamicTanhPerHead(pass, q, gamma, beta, 0.5f, headDim));

        fromViews.push_back(
            attention(pass, q, k, qkv.columns(2 * dim, dim), heads, headDim));
        fromCopies.push_back(attention(pass, q, k, v, heads, headDim));

        fromViews.push_back(bandedAttention(
            pass, q, k, qkv.columns(2 * dim, dim), heads, headDim, band));
        fromCopies.push_back(bandedAttention(pass, q, k, v, heads, headDim, band));
    }

    commands.commit();

    for (auto i = std::size_t {}; i < fromViews.size(); ++i)
        check(fromViews[i].toHostF32() == fromCopies[i].toHostF32());
};

namespace
{
// Several tensors packed into one buffer, each starting one float past the
// end of the last, so no offset but the first is a multiple of sixteen.
struct SharedTensors
{
    std::vector<float> values;
    std::vector<std::int64_t> offsets;

    void add(const std::vector<float>& tensor)
    {
        values.push_back(-1.f);
        offsets.push_back((std::int64_t) values.size()
                          * (std::int64_t) sizeof(float));
        values.insert(values.end(), tensor.begin(), tensor.end());
    }

    std::shared_ptr<const Buffer> upload(Device& device) const
    {
        return std::make_shared<const Buffer>(
            device.makeBuffer(values.data(),
                              (std::int64_t) values.size() * sizeof(float),
                              BufferUsage::Storage));
    }
};

std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return values;
}
} // namespace

auto tOffsetTensorsReadWhatOwnTensorsRead =
    test("Tensor/tensorsAtAnOffsetGiveTheBitsOfTensorsOfTheirOwn") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 40, heads = 2, headDim = 32, dim = heads * headDim;
    constexpr auto outputs = 96;

    auto inputValues = scatteredValues(rows * dim, 1);
    auto weightValues = scatteredValues(outputs * dim, 2);
    auto biasValues = scatteredValues(outputs, 3);
    auto gammaValues = scatteredValues(dim, 4);
    auto qkvValues = scatteredValues(rows * 3 * dim, 5);

    auto shared = SharedTensors {};
    shared.add(inputValues);
    shared.add(weightValues);
    shared.add(biasValues);
    shared.add(gammaValues);
    shared.add(qkvValues);

    auto buffer = shared.upload(device);
    auto at = [&](int index, std::vector<int> shape)
    {
        return Tensor {buffer,
                       shared.offsets[(std::size_t) index],
                       std::move(shape),
                       DType::F32};
    };

    auto input = at(0, {rows, dim});
    auto weight = at(1, {outputs, dim});
    auto bias = at(2, {outputs});
    auto gamma = at(3, {dim});
    auto qkv = at(4, {rows, 3 * dim});

    check(input.byteOffset() == 4);
    check(input.toHostF32() == inputValues);
    check(bias.toHostF32() == biasValues);

    auto ownInput = tensorOf(inputValues, {rows, dim});
    auto ownWeight = tensorOf(weightValues, {outputs, dim});
    auto ownBias = tensorOf(biasValues, {outputs});
    auto ownGamma = tensorOf(gammaValues, {dim});
    auto ownQkv = tensorOf(qkvValues, {rows, 3 * dim});

    auto commands = device.makeCommandBuffer();
    auto fromOffsets = std::vector<Tensor> {};
    auto fromOwn = std::vector<Tensor> {};

    {
        auto pass = commands.beginCompute();

        fromOffsets.push_back(linear(pass, input, weight, &bias));
        fromOwn.push_back(linear(pass, ownInput, ownWeight, &ownBias));

        fromOffsets.push_back(rmsNorm(pass, input, gamma, 1e-6f));
        fromOwn.push_back(rmsNorm(pass, ownInput, ownGamma, 1e-6f));

        fromOffsets.push_back(add(pass, input, input));
        fromOwn.push_back(add(pass, ownInput, ownInput));

        fromOffsets.push_back(
            rmsNormPerHead(pass, qkv.columns(dim, dim), gamma, headDim, 1e-6f));
        fromOwn.push_back(rmsNormPerHead(
            pass, ownQkv.columns(dim, dim), ownGamma, headDim, 1e-6f));

        fromOffsets.push_back(attention(
            pass, input, input, qkv.columns(2 * dim, dim), heads, headDim));
        fromOwn.push_back(attention(
            pass, ownInput, ownInput, ownQkv.columns(2 * dim, dim), heads, headDim));
    }

    commands.commit();

    for (auto i = std::size_t {}; i < fromOffsets.size(); ++i)
        check(fromOffsets[i].toHostF32() == fromOwn[i].toHostF32());

    auto flat = reshape(std::move(weight), {outputs * dim});
    check(flat.byteOffset() == shared.offsets[1]);
    check(flat.toHostF32() == weightValues);
};
