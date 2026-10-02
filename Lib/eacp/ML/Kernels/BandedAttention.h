#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
// Self-attention where row r sees only the columns
// [max(segmentStart, r - leftRadius), min(segmentEnd, r + rightRadius + 1)),
// with rows grouped into segments of segmentRows that never see each other.
// The last segment ends at the last row when the rows are not a multiple of
// segmentRows.
// One segment spanning every row is a sliding window; radii covering a whole
// segment make it block-diagonal. Scores are kept for the window only.
//
// It gives the same bits as attention() with the equivalent additive mask
// (0 inside, -1e9 outside; per segment, for a block-diagonal one): a masked
// column's exp is exactly 0, the row-stats lanes still count columns from the
// segment's start, and the weighted sum still adds columns in ascending order.
// As there, the row stats store each probability over its score, so every exp
// is evaluated once. A band with segmentRows below 1 or a negative radius
// throws std::invalid_argument before anything is dispatched.
struct AttentionBand
{
    int leftRadius = 0;
    int rightRadius = 0;
    int segmentRows = 0;
};

class BandedAttentionScoresKernel final : public GPU::ComputeProgram
{
public:
    BandedAttentionScoresKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int window);

    GPU::Uniform<GPU::InputBuffer> query;
    GPU::Uniform<GPU::InputBuffer> key;
    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> windowWidth;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> segmentRows;
    GPU::Uniform<GPU::UInt> leftRadius;
    GPU::Uniform<GPU::UInt> rightRadius;
    GPU::Uniform<GPU::Float> scale;

    EACP_SHADER(query,
                key,
                scores,
                headCount,
                headDimension,
                windowWidth,
                rowCount,
                segmentRows,
                leftRadius,
                rightRadius,
                scale)

private:
    void define() override;
};

class BandedAttentionRowStatsKernel final : public GPU::ComputeProgram
{
public:
    BandedAttentionRowStatsKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int window);

    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::OutputBuffer> rowSum;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> windowWidth;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> segmentRows;
    GPU::Uniform<GPU::UInt> leftRadius;
    GPU::Uniform<GPU::UInt> rightRadius;

    EACP_SHADER(scores,
                rowSum,
                headCount,
                windowWidth,
                rowCount,
                segmentRows,
                leftRadius,
                rightRadius)

private:
    void define() override;
};

class BandedAttentionWeightedSumKernel final : public GPU::ComputeProgram
{
public:
    BandedAttentionWeightedSumKernel();

    void dispatch(
        GPU::ComputePass& pass, int rows, int heads, int headDim, int window);

    GPU::Uniform<GPU::InputBuffer> value;
    GPU::Uniform<GPU::InputBuffer> probabilities;
    GPU::Uniform<GPU::InputBuffer> rowSum;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> windowWidth;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> segmentRows;
    GPU::Uniform<GPU::UInt> leftRadius;
    GPU::Uniform<GPU::UInt> rightRadius;
    GPU::Uniform<GPU::UInt> valueRowStride;
    GPU::Uniform<GPU::UInt> valueColumnOffset;

    EACP_SHADER(value,
                probabilities,
                rowSum,
                output,
                headCount,
                headDimension,
                windowWidth,
                rowCount,
                segmentRows,
                leftRadius,
                rightRadius,
                valueRowStride,
                valueColumnOffset)

private:
    void define() override;
};

Tensor bandedAttention(GPU::ComputePass& pass,
                       const Tensor& query,
                       const Tensor& key,
                       const TensorView& value,
                       int heads,
                       int headDim,
                       const AttentionBand& band,
                       GPU::Device& device = GPU::Device::shared());
} // namespace eacp::ML
