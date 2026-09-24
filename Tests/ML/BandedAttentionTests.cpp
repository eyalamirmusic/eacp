#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/BandedAttention.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
constexpr auto sentinelRows = 8;

std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return values;
}

// The tensor's own rows followed, in the same buffer, by rows of NaN: a kernel
// that reads a row past the last one brings a NaN into its result.
Tensor withNaNRowsAfter(const std::vector<float>& values,
                        int rows,
                        int cols,
                        Device& device)
{
    auto padded = values;
    padded.resize((std::size_t) (rows + sentinelRows) * cols,
                  std::numeric_limits<float>::quiet_NaN());

    auto buffer = std::make_shared<const Buffer>(
        device.makeBuffer(padded.data(),
                          (std::int64_t) padded.size() * sizeof(float),
                          BufferUsage::Storage));

    return Tensor {buffer, 0, {rows, cols}, DType::F32};
}

struct Window
{
    int first;
    int end;
};

Window windowOf(int row, int rows, const AttentionBand& band)
{
    auto segmentStart = row / band.segmentRows * band.segmentRows;
    auto segmentEnd = std::min(segmentStart + band.segmentRows, rows);

    return {std::max(segmentStart, row - band.leftRadius),
            std::min(segmentEnd, row + band.rightRadius + 1)};
}

std::vector<float> referenceBandedAttention(const std::vector<float>& q,
                                            const std::vector<float>& k,
                                            const std::vector<float>& v,
                                            int rows,
                                            int heads,
                                            int headDim,
                                            const AttentionBand& band)
{
    auto output = std::vector<float>((std::size_t) rows * heads * headDim);
    auto scale = 1.0 / std::sqrt((double) headDim);

    for (auto row = 0; row < rows; ++row)
        for (auto head = 0; head < heads; ++head)
        {
            auto window = windowOf(row, rows, band);
            auto scores = std::vector<double> {};

            for (auto col = window.first; col < window.end; ++col)
            {
                auto qBase = (std::size_t) (row * heads + head) * headDim;
                auto kBase = (std::size_t) (col * heads + head) * headDim;
                auto dot = 0.0;

                for (auto d = 0; d < headDim; ++d)
                    dot += (double) q[qBase + (std::size_t) d]
                           * (double) k[kBase + (std::size_t) d];

                scores.push_back(dot * scale);
            }

            auto peak = *std::max_element(scores.begin(), scores.end());
            auto total = 0.0;

            for (auto& score: scores)
            {
                score = std::exp(score - peak);
                total += score;
            }

            for (auto d = 0; d < headDim; ++d)
            {
                auto accumulator = 0.0;

                for (auto col = window.first; col < window.end; ++col)
                {
                    auto vBase = (std::size_t) (col * heads + head) * headDim;
                    accumulator += scores[(std::size_t) (col - window.first)]
                                   * (double) v[vBase + (std::size_t) d];
                }

                auto index =
                    (std::size_t) (row * heads + head) * headDim + (std::size_t) d;
                output[index] = (float) (accumulator / total);
            }
        }

    return output;
}

void checkBandMatchesReference(int rows, const AttentionBand& band)
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto heads = 2, headDim = 16, dim = heads * headDim;

    auto q = scatteredValues(rows * dim, 1);
    auto k = scatteredValues(rows * dim, 2);
    auto v = scatteredValues(rows * dim, 3);

    auto qTensor = withNaNRowsAfter(q, rows, dim, device);
    auto kTensor = withNaNRowsAfter(k, rows, dim, device);
    auto vTensor = withNaNRowsAfter(v, rows, dim, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result =
            bandedAttention(pass, qTensor, kTensor, vTensor, heads, headDim, band);
    }

    commands.commit();

    auto values = result->toHostF32();
    auto expected = referenceBandedAttention(q, k, v, rows, heads, headDim, band);

    check(values.size() == expected.size());
    check(std::none_of(
        values.begin(), values.end(), [](float x) { return std::isnan(x); }));

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= 2.0e-4f * (float) band.segmentRows);
}
} // namespace

auto tBandedLastSegmentShorterThanSegment =
    test("BandedAttention/lastPartialSegmentReadsNoRowPastTheEnd") = []
{
    checkBandMatchesReference(17,
                              {.leftRadius = 1, .rightRadius = 1, .segmentRows = 6});
};

auto tBandedLastSegmentShorterThanWindow =
    test("BandedAttention/lastSegmentShorterThanTheWindow") = []
{
    checkBandMatchesReference(11,
                              {.leftRadius = 2, .rightRadius = 2, .segmentRows = 4});
};

auto tBandedBlockDiagonalPartialSegment =
    test("BandedAttention/blockDiagonalWithAPartialLastSegment") = []
{
    checkBandMatchesReference(10,
                              {.leftRadius = 3, .rightRadius = 3, .segmentRows = 4});
};

auto tBandedWholeSegments = test("BandedAttention/wholeSegmentsMatchReference") = []
{
    checkBandMatchesReference(12,
                              {.leftRadius = 2, .rightRadius = 1, .segmentRows = 4});
};
