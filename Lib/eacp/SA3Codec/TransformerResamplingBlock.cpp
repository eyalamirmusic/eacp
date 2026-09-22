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
    auto chunkCount = input.rows() / effectiveChunkSize;
    auto output = Tensor::uninitializedF32(input.shape(), device);

    for (auto chunk = 0; chunk < chunkCount; ++chunk)
    {
        auto chunkInput = sliceRowsGpu(pass, input, chunk * effectiveChunkSize, effectiveChunkSize, device);

        for (auto layerIndex = layerStart; layerIndex < layerEnd; ++layerIndex)
            chunkInput =
                applyCodecTransformerBlock(pass, chunkInput, layers[(std::size_t) layerIndex], device);

        writeRowsIntoGpu(pass, output, chunk * effectiveChunkSize, chunkInput, device);
    }

    return output;
}
}

Tensor applyTransformerResamplingBlock(ComputePass& pass,
                                      const Tensor& input,
                                      const ResamplingBlockWeights& weights,
                                      Device& device)
{
    auto x = std::optional<Tensor> {};

    if (weights.isEncoder)
    {
        auto padded = zeroPadRowsGpu(pass, input, weights.chunkSize, device);
        x = applyWNConv1d(pass, padded, weights.mapping, device);
    }
    else
    {
        auto padModulo = weights.chunkSize / weights.stride;
        x = zeroPadRowsGpu(pass, input, padModulo, device);
    }

    auto inputSegSize = weights.isEncoder ? weights.stride : 1;
    auto outputSegSize = weights.isEncoder ? 1 : weights.stride;
    auto subChunkSize = weights.stride + 1;

    auto folded = foldWithNewTokens(pass, *x, inputSegSize, outputSegSize, weights.newTokens, device);

    auto effectiveChunkSize = weights.chunkSize + weights.chunkSize / weights.stride;
    auto split = weights.transformerDepth / 2;
    auto shift = effectiveChunkSize / 2;

    auto firstOut = runChunkedStack(pass, folded, effectiveChunkSize, weights.layers, 0, split, device);

    auto headPad = sliceRowsGpu(pass, firstOut, 0, shift, device);
    auto tailPad = sliceRowsGpu(pass, firstOut, firstOut.rows() - shift, shift, device);
    auto padded = concatRowsGpu(pass, headPad, firstOut, tailPad, device);

    auto secondOut = runChunkedStack(
        pass, padded, effectiveChunkSize, weights.layers, split, weights.transformerDepth, device);

    auto sliced = sliceRowsGpu(pass, secondOut, shift, firstOut.rows(), device);

    auto unfolded = unfoldLastSegment(pass, sliced, subChunkSize, outputSegSize, device);

    if (weights.isEncoder)
        return unfolded;

    return applyWNConv1d(pass, unfolded, weights.mapping, device);
}
}
