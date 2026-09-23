#include "TransformerResamplingBlock.h"

#include "GpuOps.h"

#include <optional>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

Tensor foldWithNewTokens(ComputePass& pass,
                        const Tensor& input,
                        int inputSegSize,
                        int outputSegSize,
                        const Tensor& newTokens,
                        Device& device)
{
    return foldWithNewTokensGpu(pass, input, inputSegSize, outputSegSize, newTokens, device);
}

Tensor unfoldLastSegment(ComputePass& pass,
                        const Tensor& input,
                        int subChunkSize,
                        int outputSegSize,
                        Device& device)
{
    return unfoldLastSegmentGpu(pass, input, subChunkSize, outputSegSize, device);
}

namespace
{
Tensor runChunkedStack(ComputePass& pass,
                       const Tensor& input,
                       int effectiveChunkSize,
                       const std::vector<CodecBlockWeights>& layers,
                       int layerStart,
                       int layerEnd,
                       Device& device)
{
    auto chunkedRows = input.rows() / effectiveChunkSize * effectiveChunkSize;
    auto band =
        AttentionBand {effectiveChunkSize, effectiveChunkSize, effectiveChunkSize};

    auto whole = chunkedRows == input.rows();
    auto sliced = whole ? std::nullopt
                        : std::optional<Tensor> {
                              sliceRowsGpu(pass, input, 0, chunkedRows, device)};
    const auto& source = whole ? input : *sliced;
    auto x = std::optional<Tensor> {};

    for (auto layerIndex = layerStart; layerIndex < layerEnd; ++layerIndex)
        x = applyCodecTransformerBlock(pass,
                                       x.has_value() ? *x : source,
                                       layers[(std::size_t) layerIndex],
                                       band,
                                       device);

    if (whole)
        return std::move(*x);

    auto output = Tensor::uninitializedF32(input.shape(), device);
    writeRowsIntoGpu(pass, output, 0, *x, device);
    return output;
}

Tensor runSlidingWindowStack(const Tensor& input,
                             const std::vector<CodecBlockWeights>& layers,
                             int leftRadius,
                             int rightRadius,
                             Device& device)
{
    auto band = AttentionBand {leftRadius, rightRadius, input.rows()};
    auto x = std::optional<Tensor> {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();

        for (const auto& layer: layers)
            x = applyCodecTransformerBlock(
                pass, x.has_value() ? *x : input, layer, band, device);
    }

    commands.commit();
    return std::move(*x);
}

Tensor applyChunkMidpointShift(ComputePass& pass,
                               const Tensor& folded,
                               const ResamplingBlockWeights& weights,
                               Device& device)
{
    auto effectiveChunkSize = weights.chunkSize + weights.chunkSize / weights.stride;
    auto split = weights.transformerDepth / 2;
    auto shift = effectiveChunkSize / 2;

    auto firstOut = runChunkedStack(pass, folded, effectiveChunkSize, weights.layers, 0, split, device);

    auto headPad = sliceRowsGpu(pass, firstOut, 0, shift, device);
    auto tailPad = sliceRowsGpu(pass, firstOut, firstOut.rows() - shift, shift, device);
    auto padded = concatRowsGpu(pass, headPad, firstOut, tailPad, device);

    auto secondOut = runChunkedStack(
        pass, padded, effectiveChunkSize, weights.layers, split, weights.transformerDepth, device);

    return sliceRowsGpu(pass, secondOut, shift, firstOut.rows(), device);
}
}

Tensor applyTransformerResamplingBlock(const Tensor& input,
                                      const ResamplingBlockWeights& weights,
                                      Device& device)
{
    auto inputSegSize = weights.isEncoder ? weights.stride : 1;
    auto outputSegSize = weights.isEncoder ? 1 : weights.stride;
    auto subChunkSize = weights.stride + 1;

    auto padModulo = weights.attentionMode == CodecAttentionMode::ChunkMidpointShift
                       ? weights.chunkSize
                       : inputSegSize;

    auto folded = std::optional<Tensor> {};

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            auto x = std::optional<Tensor> {};

            if (weights.isEncoder)
            {
                auto padded = zeroPadRowsGpu(pass, input, padModulo, device);
                x = applyWNConv1d(pass, padded, weights.mapping, device);
            }
            else
            {
                auto decoderPadModulo = weights.attentionMode == CodecAttentionMode::ChunkMidpointShift
                                          ? weights.chunkSize / weights.stride
                                          : inputSegSize;
                x = zeroPadRowsGpu(pass, input, decoderPadModulo, device);
            }

            folded = foldWithNewTokens(pass, *x, inputSegSize, outputSegSize, weights.newTokens, device);
        }

        commands.commit();
    }

    auto stacked = std::optional<Tensor> {};

    if (weights.attentionMode == CodecAttentionMode::ChunkMidpointShift)
    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            stacked = applyChunkMidpointShift(pass, *folded, weights, device);
        }

        commands.commit();
    }
    else
    {
        auto radius = weights.slidingWindowRadiusChunks * subChunkSize;
        stacked = runSlidingWindowStack(*folded, weights.layers, radius, radius, device);
    }

    auto result = std::optional<Tensor> {};

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            auto unfolded = unfoldLastSegment(pass, *stacked, subChunkSize, outputSegSize, device);

            result = weights.isEncoder ? std::move(unfolded)
                                       : applyWNConv1d(pass, unfolded, weights.mapping, device);
        }

        commands.commit();
    }

    return std::move(*result);
}
}
