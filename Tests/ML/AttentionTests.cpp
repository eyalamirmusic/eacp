#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Attention.h>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return values;
}

std::vector<float> rmsNormalizeHeads(const std::vector<float>& x,
                                     const std::vector<float>& gamma,
                                     int rowCount,
                                     int heads,
                                     int headDim,
                                     float eps)
{
    auto result = x;

    for (auto rowHead = 0; rowHead < rowCount * heads; ++rowHead)
    {
        auto base = (std::size_t) rowHead * headDim;
        auto sumSquares = 0.0;

        for (auto d = 0; d < headDim; ++d)
        {
            auto value = (double) x[base + (std::size_t) d];
            sumSquares += value * value;
        }

        auto scale = 1.0 / std::sqrt(sumSquares / headDim + (double) eps);

        for (auto d = 0; d < headDim; ++d)
            result[base + (std::size_t) d] =
                (float) ((double) x[base + (std::size_t) d] * scale
                         * (double) gamma[(std::size_t) d]);
    }

    return result;
}

std::vector<float> referenceAttention(const std::vector<float>& q,
                                      const std::vector<float>& k,
                                      const std::vector<float>& v,
                                      int rows,
                                      int cols,
                                      int heads,
                                      int headDim,
                                      const std::vector<float>* additiveMask,
                                      bool causal)
{
    auto output = std::vector<float>((std::size_t) rows * heads * headDim);
    auto scale = 1.f / std::sqrt((float) headDim);

    for (auto row = 0; row < rows; ++row)
        for (auto head = 0; head < heads; ++head)
        {
            auto scores = std::vector<float>((std::size_t) cols);

            for (auto col = 0; col < cols; ++col)
            {
                auto qBase = (std::size_t) (row * heads + head) * headDim;
                auto kBase = (std::size_t) (col * heads + head) * headDim;

                auto dot = 0.0;

                for (auto d = 0; d < headDim; ++d)
                    dot += (double) q[qBase + (std::size_t) d]
                           * (double) k[kBase + (std::size_t) d];

                auto score = (float) dot * scale;

                if (additiveMask != nullptr)
                    score += (*additiveMask)[(std::size_t) (row * cols + col)];

                if (causal && col > row)
                    score = -1.0e9f;

                scores[(std::size_t) col] = score;
            }

            auto peak = -std::numeric_limits<float>::infinity();

            for (auto value: scores)
                peak = std::max(peak, value);

            auto total = 0.0;

            for (auto& value: scores)
            {
                value = std::exp(value - peak);
                total += (double) value;
            }

            for (auto d = 0; d < headDim; ++d)
            {
                auto accumulator = 0.0;

                for (auto col = 0; col < cols; ++col)
                {
                    auto vBase = (std::size_t) (col * heads + head) * headDim;
                    accumulator += (double) scores[(std::size_t) col]
                                   * (double) v[vBase + (std::size_t) d];
                }

                auto index =
                    (std::size_t) (row * heads + head) * headDim + (std::size_t) d;

                output[index] = (float) (accumulator / total);
            }
        }

    return output;
}

void checkMatches(const std::vector<float>& values,
                  const std::vector<float>& expected,
                  float tolerance)
{
    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= tolerance);
}
} // namespace

auto tAttentionMatchesReferencePlain = test("Attention/matchesReferencePlain") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;
    constexpr auto cols = 7;
    constexpr auto heads = 4;
    constexpr auto headDim = 32;

    auto q = scatteredValues(rows * heads * headDim, 1);
    auto k = scatteredValues(cols * heads * headDim, 2);
    auto v = scatteredValues(cols * heads * headDim, 3);

    auto qTensor = Tensor::fromHostF32(q.data(), {rows, heads * headDim}, device);
    auto kTensor = Tensor::fromHostF32(k.data(), {cols, heads * headDim}, device);
    auto vTensor = Tensor::fromHostF32(v.data(), {cols, heads * headDim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = attention(pass, qTensor, kTensor, vTensor, heads, headDim);
    }

    commands.commit();

    checkMatches(
        result->toHostF32(),
        referenceAttention(q, k, v, rows, cols, heads, headDim, nullptr, false),
        2.0e-4f * cols);
};

auto tAttentionMatchesReferenceCausal = test("Attention/matchesReferenceCausal") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 6;
    constexpr auto cols = 6;
    constexpr auto heads = 2;
    constexpr auto headDim = 16;

    auto q = scatteredValues(rows * heads * headDim, 5);
    auto k = scatteredValues(cols * heads * headDim, 7);
    auto v = scatteredValues(cols * heads * headDim, 11);

    auto qTensor = Tensor::fromHostF32(q.data(), {rows, heads * headDim}, device);
    auto kTensor = Tensor::fromHostF32(k.data(), {cols, heads * headDim}, device);
    auto vTensor = Tensor::fromHostF32(v.data(), {cols, heads * headDim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};
    auto mask = buildCausalMask(rows, cols, device);

    {
        auto pass = commands.beginCompute();

        result = attention(
            pass, qTensor, kTensor, vTensor, heads, headDim, {.mask = &mask});
    }

    commands.commit();

    checkMatches(
        result->toHostF32(),
        referenceAttention(q, k, v, rows, cols, heads, headDim, nullptr, true),
        2.0e-4f * cols);
};

auto tAttentionMatchesReferenceWithAdditiveMask =
    test("Attention/matchesReferenceWithAdditiveMask") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 4;
    constexpr auto cols = 9;
    constexpr auto heads = 3;
    constexpr auto headDim = 16;

    auto q = scatteredValues(rows * heads * headDim, 13);
    auto k = scatteredValues(cols * heads * headDim, 17);
    auto v = scatteredValues(cols * heads * headDim, 19);

    auto maskValues = std::vector<float>((std::size_t) rows * cols, 0.f);

    for (auto row = 0; row < rows; ++row)
        maskValues[(std::size_t) (row * cols + (cols - 1))] = -1.0e9f;

    auto qTensor = Tensor::fromHostF32(q.data(), {rows, heads * headDim}, device);
    auto kTensor = Tensor::fromHostF32(k.data(), {cols, heads * headDim}, device);
    auto vTensor = Tensor::fromHostF32(v.data(), {cols, heads * headDim}, device);
    auto maskTensor = Tensor::fromHostF32(maskValues.data(), {rows, cols}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();

        result = attention(
            pass, qTensor, kTensor, vTensor, heads, headDim, {.mask = &maskTensor});
    }

    commands.commit();

    checkMatches(
        result->toHostF32(),
        referenceAttention(q, k, v, rows, cols, heads, headDim, &maskValues, false),
        2.0e-4f * cols);
};

auto tAttentionMatchesReferenceWithQKNorm =
    test("Attention/matchesReferenceWithQKNorm") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 4;
    constexpr auto cols = 5;
    constexpr auto heads = 2;
    constexpr auto headDim = 16;
    constexpr auto eps = 1.0e-6f;

    auto q = scatteredValues(rows * heads * headDim, 23);
    auto k = scatteredValues(cols * heads * headDim, 29);
    auto v = scatteredValues(cols * heads * headDim, 31);
    auto qGamma = scatteredValues(headDim, 37);
    auto kGamma = scatteredValues(headDim, 41);

    auto qTensor = Tensor::fromHostF32(q.data(), {rows, heads * headDim}, device);
    auto kTensor = Tensor::fromHostF32(k.data(), {cols, heads * headDim}, device);
    auto vTensor = Tensor::fromHostF32(v.data(), {cols, heads * headDim}, device);
    auto qGammaTensor = Tensor::fromHostF32(qGamma.data(), {headDim}, device);
    auto kGammaTensor = Tensor::fromHostF32(kGamma.data(), {headDim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();

        result = attention(pass,
                           qTensor,
                           kTensor,
                           vTensor,
                           heads,
                           headDim,
                           {.queryNorm = &qGammaTensor,
                            .keyNorm = &kGammaTensor,
                            .normEpsilon = eps},
                           device);
    }

    commands.commit();

    auto normalizedQ = rmsNormalizeHeads(q, qGamma, rows, heads, headDim, eps);
    auto normalizedK = rmsNormalizeHeads(k, kGamma, cols, heads, headDim, eps);

    checkMatches(
        result->toHostF32(),
        referenceAttention(
            normalizedQ, normalizedK, v, rows, cols, heads, headDim, nullptr, false),
        2.0e-4f * cols);
};

auto tAttendWithScoresLeavesProbabilitiesBehind =
    test("Attention/attendWithScoresLeavesProbabilitiesBehind") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;
    constexpr auto cols = 300;
    constexpr auto heads = 2;
    constexpr auto headDim = 8;

    auto scoreValues = scatteredValues(rows * heads * cols, 4);
    auto valueValues = scatteredValues(cols * heads * headDim, 6);

    auto scores =
        Tensor::fromHostF32(scoreValues.data(), {rows, heads, cols}, device);
    auto value =
        Tensor::fromHostF32(valueValues.data(), {cols, heads * headDim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = attendWithScores(pass, scores, value, heads, headDim, device);
    }

    commands.commit();

    auto probabilities = scores.toHostF32();
    auto expectedProbabilities = std::vector<float> {};
    auto expectedOutput = std::vector<float> {};

    for (auto rowHead = 0; rowHead < rows * heads; ++rowHead)
    {
        auto base = (std::size_t) rowHead * cols;
        auto peak = scoreValues[base];

        for (auto col = 0; col < cols; ++col)
            peak = std::max(peak, scoreValues[base + (std::size_t) col]);

        auto total = 0.0;

        for (auto col = 0; col < cols; ++col)
        {
            auto p = std::exp(scoreValues[base + (std::size_t) col] - peak);
            expectedProbabilities.push_back(p);
            total += (double) p;
        }

        auto head = rowHead % heads;

        for (auto d = 0; d < headDim; ++d)
        {
            auto accumulator = 0.0;

            for (auto col = 0; col < cols; ++col)
                accumulator +=
                    (double) expectedProbabilities[base + (std::size_t) col]
                    * (double) valueValues[(
                        std::size_t) ((col * heads + head) * headDim + d)];

            expectedOutput.push_back((float) (accumulator / total));
        }
    }

    checkMatches(probabilities, expectedProbabilities, 1.0e-5f);
    checkMatches(result->toHostF32(), expectedOutput, 1.0e-4f);
};
