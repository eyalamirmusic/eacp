#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Tensor/Tensor.h>
#include <Codec/HostMatrix.h>
#include <Codec/TransformerResamplingBlock.h>
#include <Codec/WNConv1d.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3Codec;

namespace
{
void checkClose(float value, float expected, float tolerance)
{
    check(std::abs(value - expected) <= tolerance);
}

HostMatrix rangeMatrix(int rows, int cols, float salt)
{
    auto matrix = HostMatrix::zeros(rows, cols);

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < cols; ++c)
            matrix.at(r, c) = salt + (float) (r * cols + c);

    return matrix;
}

HostMatrix
    runOnDevice(Device& device,
                const std::vector<HostMatrix>& inputs,
                std::function<Tensor(ComputePass&, const std::vector<Tensor>&)> body)
{
    auto inputTensors = std::vector<Tensor> {};

    for (const auto& input: inputs)
        inputTensors.push_back(Tensor::fromHostF32(
            input.data.data(), {input.rows, input.cols}, device));

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = body(pass, inputTensors);
    }

    commands.commit();

    return HostMatrix {result->toHostF32(), result->rows(), result->cols()};
}
} // namespace

auto tZeroPadNoOpWhenAlreadyMultiple =
    test("SA3Codec/zeroPadRowsToMultipleIsNoOpWhenAligned") = []
{
    auto input = rangeMatrix(4, 3, 0.f);
    auto padded = zeroPadRowsToMultiple(input, 4);

    check(padded.rows == 4);
    check(padded.cols == 3);

    for (auto i = std::size_t {}; i < input.data.size(); ++i)
        checkClose(padded.data[i], input.data[i], 0.f);
};

auto tZeroPadAddsZeroRows = test("SA3Codec/zeroPadRowsToMultipleAddsZeroRows") = []
{
    auto input = rangeMatrix(5, 2, 1.f);
    auto padded = zeroPadRowsToMultiple(input, 4);

    check(padded.rows == 8);

    for (auto c = 0; c < 2; ++c)
        checkClose(padded.at(4, c), input.at(4, c), 0.f);

    for (auto r = 5; r < 8; ++r)
        for (auto c = 0; c < 2; ++c)
            checkClose(padded.at(r, c), 0.f, 0.f);
};

auto tExtractAndWriteRowsRoundTrip =
    test("SA3Codec/extractAndWriteRowsRoundTrip") = []
{
    auto input = rangeMatrix(6, 3, 10.f);
    auto middle = extractRows(input, 2, 3);

    check(middle.rows == 3);

    auto rebuilt = HostMatrix::zeros(6, 3);
    writeRows(rebuilt, 2, middle);

    for (auto c = 0; c < 3; ++c)
        checkClose(rebuilt.at(3, c), input.at(3, c), 0.f);
};

auto tConcatRowsStacksInOrder = test("SA3Codec/concatRowsStacksInOrder") = []
{
    auto a = rangeMatrix(2, 2, 0.f);
    auto b = rangeMatrix(3, 2, 100.f);
    auto c = rangeMatrix(1, 2, 200.f);

    auto result = concatRows(a, b, c);

    check(result.rows == 6);
    checkClose(result.at(0, 0), a.at(0, 0), 0.f);
    checkClose(result.at(2, 0), b.at(0, 0), 0.f);
    checkClose(result.at(5, 0), c.at(0, 0), 0.f);
};

auto tAddAndSubtractMatricesAreInverse =
    test("SA3Codec/addAndSubtractMatricesAreInverses") = []
{
    auto a = rangeMatrix(3, 3, 5.f);
    auto b = rangeMatrix(3, 3, -2.f);

    auto sum = addMatrices(a, b);
    auto back = subtractMatrices(sum, b);

    for (auto i = std::size_t {}; i < a.data.size(); ++i)
        checkClose(back.data[i], a.data[i], 1.0e-5f);
};

auto tSliceColumnsExtractsSubrange =
    test("SA3Codec/sliceColumnsExtractsSubrange") = []
{
    auto input = rangeMatrix(2, 6, 0.f);
    auto slice = sliceColumns(input, 2, 3);

    check(slice.rows == 2);
    check(slice.cols == 3);

    for (auto r = 0; r < 2; ++r)
        for (auto c = 0; c < 3; ++c)
            checkClose(slice.at(r, c), input.at(r, 2 + c), 0.f);
};

auto tFoldWithNewTokensEncoderDirection =
    test("SA3Codec/foldWithNewTokensMatchesHandComputedEncoderCase") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = rangeMatrix(4, 2, 0.f);
    auto newTokens = HostMatrix {std::vector<float> {9.f, 9.f}, 1, 2};

    auto folded = runOnDevice(
        device,
        {input, newTokens},
        [](ComputePass& pass, const std::vector<Tensor>& tensors)
        { return foldWithNewTokens(pass, tensors[0], 2, 1, tensors[1]); });

    check(folded.rows == 6);
    check(folded.cols == 2);

    checkClose(folded.at(0, 0), input.at(0, 0), 0.f);
    checkClose(folded.at(1, 0), input.at(1, 0), 0.f);
    checkClose(folded.at(2, 0), 9.f, 0.f);
    checkClose(folded.at(3, 0), input.at(2, 0), 0.f);
    checkClose(folded.at(4, 0), input.at(3, 0), 0.f);
    checkClose(folded.at(5, 0), 9.f, 0.f);
};

auto tFoldWithNewTokensDecoderDirection =
    test("SA3Codec/foldWithNewTokensMatchesHandComputedDecoderCase") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = rangeMatrix(2, 1, 0.f);
    auto newTokens = HostMatrix {std::vector<float> {7.f}, 1, 1};

    auto folded = runOnDevice(
        device,
        {input, newTokens},
        [](ComputePass& pass, const std::vector<Tensor>& tensors)
        { return foldWithNewTokens(pass, tensors[0], 1, 2, tensors[1]); });

    check(folded.rows == 6);

    checkClose(folded.at(0, 0), input.at(0, 0), 0.f);
    checkClose(folded.at(1, 0), 7.f, 0.f);
    checkClose(folded.at(2, 0), 7.f, 0.f);
    checkClose(folded.at(3, 0), input.at(1, 0), 0.f);
    checkClose(folded.at(4, 0), 7.f, 0.f);
    checkClose(folded.at(5, 0), 7.f, 0.f);
};

auto tUnfoldLastSegmentEncoderDirection =
    test("SA3Codec/unfoldLastSegmentTakesTrailingTokenForEncoder") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = rangeMatrix(6, 1, 0.f);

    auto unfolded =
        runOnDevice(device,
                    {input},
                    [](ComputePass& pass, const std::vector<Tensor>& tensors)
                    { return unfoldLastSegment(pass, tensors[0], 3, 1); });

    check(unfolded.rows == 2);
    checkClose(unfolded.at(0, 0), input.at(2, 0), 0.f);
    checkClose(unfolded.at(1, 0), input.at(5, 0), 0.f);
};

auto tUnfoldLastSegmentDecoderDirection =
    test("SA3Codec/unfoldLastSegmentTakesTrailingSpanForDecoder") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = rangeMatrix(6, 1, 0.f);

    auto unfolded =
        runOnDevice(device,
                    {input},
                    [](ComputePass& pass, const std::vector<Tensor>& tensors)
                    { return unfoldLastSegment(pass, tensors[0], 3, 2); });

    check(unfolded.rows == 4);
    checkClose(unfolded.at(0, 0), input.at(1, 0), 0.f);
    checkClose(unfolded.at(1, 0), input.at(2, 0), 0.f);
    checkClose(unfolded.at(2, 0), input.at(4, 0), 0.f);
    checkClose(unfolded.at(3, 0), input.at(5, 0), 0.f);
};

auto tFoldThenUnfoldRoundTripsRealSegment =
    test("SA3Codec/foldThenUnfoldRecoversRealSegmentUntouchedByAttention") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = rangeMatrix(4, 2, 1.f);
    auto newTokens = HostMatrix {std::vector<float> {-1.f, -1.f}, 1, 2};

    auto unfolded =
        runOnDevice(device,
                    {input, newTokens},
                    [](ComputePass& pass, const std::vector<Tensor>& tensors)
                    {
                        auto folded =
                            foldWithNewTokens(pass, tensors[0], 2, 1, tensors[1]);
                        return unfoldLastSegment(pass, folded, 3, 1);
                    });

    check(unfolded.rows == 2);

    for (auto c = 0; c < 2; ++c)
    {
        checkClose(unfolded.at(0, 0), -1.f, 0.f);
        checkClose(unfolded.at(0, 1), -1.f, 0.f);
    }
};

auto tComputeWeightNormFlatMatchesHandComputedNorm =
    test("SA3Codec/computeWeightNormFlatMatchesHandComputedNorm") = []
{
    float gain[2] = {2.f, 3.f};
    float direction[2 * 3] = {3.f, 4.f, 0.f, 1.f, 0.f, 0.f};

    auto flat = computeWeightNormFlat(gain, direction, 2, 3);

    check((int) flat.size() == 6);

    checkClose(flat[0], 2.f * 3.f / 5.f, 1.0e-5f);
    checkClose(flat[1], 2.f * 4.f / 5.f, 1.0e-5f);
    checkClose(flat[2], 0.f, 1.0e-5f);

    checkClose(flat[3], 3.f * 1.f / 1.f, 1.0e-5f);
    checkClose(flat[4], 0.f, 1.0e-5f);
    checkClose(flat[5], 0.f, 1.0e-5f);
};

auto tApplyWNConv1dKernelOneActsAsPerTimestepLinear =
    test("SA3Codec/applyWNConv1dKernelOneMatchesHandComputedLinear") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    float gain[2] = {1.f, 1.f};
    float direction[2 * 2] = {1.f, 0.f, 0.f, 1.f};
    auto flat = computeWeightNormFlat(gain, direction, 2, 2);

    auto weights = WNConv1dWeights {
        .flatWeight = Tensor::fromHostF32(flat.data(), {2, 2}, device),
        .bias = Tensor::fromHostF32(
            std::vector<float> {0.5f, -0.5f}.data(), {2}, device),
        .hasBias = true,
        .inChannels = 2,
        .outChannels = 2,
        .kernelSize = 1,
    };

    auto input = HostMatrix {std::vector<float> {1.f, 2.f, 3.f, 4.f}, 2, 2};

    auto output =
        runOnDevice(device,
                    {input},
                    [&](ComputePass& pass, const std::vector<Tensor>& tensors)
                    { return applyWNConv1d(pass, tensors[0], weights, device); });

    check(output.rows == 2);
    check(output.cols == 2);

    checkClose(output.at(0, 0), 1.f + 0.5f, 1.0e-4f);
    checkClose(output.at(0, 1), 2.f - 0.5f, 1.0e-4f);
    checkClose(output.at(1, 0), 3.f + 0.5f, 1.0e-4f);
    checkClose(output.at(1, 1), 4.f - 0.5f, 1.0e-4f);
};

auto tApplyWNConv1dKernelThreeSumsNeighbours =
    test("SA3Codec/applyWNConv1dKernelThreeMatchesHandComputedWindow") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    float gain[1] = {1.f};
    float direction[3] = {1.f, 1.f, 1.f};
    auto flat = computeWeightNormFlat(gain, direction, 1, 3);
    auto normValue = std::sqrt(3.f);

    auto weights = WNConv1dWeights {
        .flatWeight = Tensor::fromHostF32(flat.data(), {1, 3}, device),
        .bias = Tensor::fromHostF32(std::vector<float> {0.f}.data(), {1}, device),
        .hasBias = false,
        .inChannels = 1,
        .outChannels = 1,
        .kernelSize = 3,
    };

    auto input = HostMatrix {std::vector<float> {1.f, 2.f, 3.f, 4.f}, 4, 1};

    auto output =
        runOnDevice(device,
                    {input},
                    [&](ComputePass& pass, const std::vector<Tensor>& tensors)
                    { return applyWNConv1d(pass, tensors[0], weights, device); });

    check(output.rows == 4);

    checkClose(output.at(0, 0), (0.f + 1.f + 2.f) / normValue, 1.0e-4f);
    checkClose(output.at(1, 0), (1.f + 2.f + 3.f) / normValue, 1.0e-4f);
    checkClose(output.at(2, 0), (2.f + 3.f + 4.f) / normValue, 1.0e-4f);
    checkClose(output.at(3, 0), (3.f + 4.f + 0.f) / normValue, 1.0e-4f);
};

auto tDynamicTanhMatchesHandComputedFormula =
    test("SA3Codec/dynamicTanhViaMlMatchesHandComputedFormula") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto alpha = 2.f;
    auto gammaValues = std::vector<float> {1.5f, -1.0f};
    auto betaValues = std::vector<float> {0.25f, 0.5f};
    auto inputValues = std::vector<float> {0.3f, -0.7f};

    auto inputTensor = Tensor::fromHostF32(inputValues.data(), {1, 2}, device);
    auto gammaTensor = Tensor::fromHostF32(gammaValues.data(), {2}, device);
    auto betaTensor = Tensor::fromHostF32(betaValues.data(), {2}, device);

    auto commands = device.makeCommandBuffer();
    auto resultTensor = Tensor::uninitializedF32({1, 2}, device);

    {
        auto pass = commands.beginCompute();
        resultTensor =
            dynamicTanh(pass, inputTensor, gammaTensor, betaTensor, alpha, device);
    }

    commands.commit();

    auto result = resultTensor.toHostF32();

    for (auto i = 0; i < 2; ++i)
    {
        auto expected = gammaValues[(std::size_t) i]
                            * std::tanh(alpha * inputValues[(std::size_t) i])
                        + betaValues[(std::size_t) i];
        checkClose(result[(std::size_t) i], expected, 1.0e-5f);
    }
};

// The reference cannot fold a partial chunk and neither can this: a row count
// off the chunk grid is refused rather than run with its tail never written.
auto tChunkedStackRefusesPartialChunk =
    test("SA3Codec/chunkedStackRefusesAPartialChunk") = []
{
    auto refuses = [](int rows, int chunk)
    {
        try
        {
            checkChunkedRows(rows, chunk);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }

        return false;
    };

    check(!refuses(68, 34));
    check(!refuses(0, 34));
    check(refuses(69, 34));
    check(refuses(33, 34));
    check(refuses(34, 0));
};
