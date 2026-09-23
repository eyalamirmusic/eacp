#include "CodecTransformerBlock.h"

#include "GpuOps.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/BandedAttention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/SwiGLU.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
Tensor dynamicTanhPerHead(ComputePass& pass,
                          const Tensor& input,
                          const DynamicTanhWeights& norm,
                          int rowCount,
                          int heads,
                          int headDim,
                          Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::cachedKernel<DynamicTanhKernel>(device);
    kernel.input = input.buffer();
    kernel.gamma = norm.gamma.buffer();
    kernel.beta = norm.beta.buffer();
    kernel.output = result.buffer();
    kernel.alpha = norm.alpha;
    kernel.dispatch(pass, rowCount * heads, headDim);

    return result;
}

class SegmentRoPEKernel final : public ComputeProgram
{
public:
    SegmentRoPEKernel() { compile(); }

    void dispatch(ComputePass& pass, int rows, int heads, int headDim)
    {
        headCount = (std::uint32_t) heads;
        headDimension = (std::uint32_t) headDim;
        pass.dispatch(*this, rows * heads * headDim);
    }

    Uniform<InputBuffer> input;
    Uniform<InputBuffer> invFreq;
    Uniform<OutputBuffer> output;
    Uniform<UInt> headCount;
    Uniform<UInt> headDimension;
    Uniform<UInt> halfRotaryDimension;
    Uniform<UInt> segmentRows;

    EACP_SHADER(input,
                invFreq,
                output,
                headCount,
                headDimension,
                halfRotaryDimension,
                segmentRows)

private:
    void define() override
    {
        auto i = threadId();
        auto d = i % headDimension;
        auto rowHead = i / headDimension;
        auto row = (rowHead / headCount) % segmentRows;

        auto rotaryDimension = halfRotaryDimension * 2u;
        auto base = rowHead * headDimension;

        ifThen(
            d < rotaryDimension,
            [&]
            {
                auto isFirstHalf = d < halfRotaryDimension;
                auto freqIndex = select(isFirstHalf, d, d - halfRotaryDimension);

                auto angle = toFloat(row) * invFreq[freqIndex];
                auto cosine = cos(angle);
                auto sine = sin(angle);

                auto x1 = input[base + freqIndex];
                auto x2 = input[base + halfRotaryDimension + freqIndex];

                auto rotated = select(
                    isFirstHalf, x1 * cosine - x2 * sine, x2 * cosine + x1 * sine);

                write(output, base + d, rotated);
            },
            [&] { write(output, base + d, input[base + d]); });
    }
};

Tensor applySegmentRoPE(ComputePass& pass,
                        const Tensor& input,
                        const Tensor& invFreq,
                        int heads,
                        int headDim,
                        int segmentRows,
                        Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::cachedKernel<SegmentRoPEKernel>(device);
    kernel.input = input.buffer();
    kernel.invFreq = invFreq.buffer();
    kernel.output = result.buffer();
    kernel.halfRotaryDimension = (std::uint32_t) invFreq.count();
    kernel.segmentRows = (std::uint32_t) segmentRows;
    kernel.dispatch(pass, input.rows(), heads, headDim);

    return result;
}

class SinGateKernel final : public ComputeProgram
{
public:
    SinGateKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int inner)
    {
        innerDimension = (std::uint32_t) inner;
        pass.dispatch(*this, rows * inner);
    }

    Uniform<InputBuffer> hidden;
    Uniform<OutputBuffer> output;
    Uniform<UInt> innerDimension;

    EACP_SHADER(hidden, output, innerDimension)

private:
    void define() override
    {
        auto i = threadId();
        auto col = i % innerDimension;
        auto row = i / innerDimension;

        auto base = row * innerDimension * 2u;
        auto a = hidden[base + col];
        auto b = hidden[base + innerDimension + col];

        write(output, i, a * sin(b * 3.14159265359f));
    }
};

Tensor sinGatedFeedForward(ComputePass& pass,
                          const Tensor& input,
                          const Tensor& proj0Weight,
                          const Tensor& proj0Bias,
                          const Tensor& proj2Weight,
                          const Tensor& proj2Bias,
                          Device& device)
{
    auto rows = input.rows();
    auto inner = proj0Weight.dim(0) / 2;

    auto hidden = linear(pass, input, proj0Weight, &proj0Bias, device);
    auto gated = Tensor::uninitializedF32({rows, inner}, device);

    auto& gateKernel = GPU::cachedKernel<SinGateKernel>(device);
    gateKernel.hidden = hidden.buffer();
    gateKernel.output = gated.buffer();
    gateKernel.dispatch(pass, rows, inner);

    return linear(pass, gated, proj2Weight, &proj2Bias, device);
}
}

Tensor applyCodecTransformerBlock(ComputePass& pass,
                                  const Tensor& input,
                                  const CodecBlockWeights& weights,
                                  const AttentionBand& band,
                                  Device& device)
{
    auto dim = weights.heads * weights.headDim;
    auto rows = input.rows();

    auto normed = dynamicTanh(
        pass, input, weights.preNorm.gamma, weights.preNorm.beta, weights.preNorm.alpha, device);
    auto qkv = linear(pass, normed, weights.qkvWeight, nullptr, device);

    auto qTensor = sliceColumnsGpu(pass, qkv, 0 * dim, dim, device);
    auto kTensor = sliceColumnsGpu(pass, qkv, 1 * dim, dim, device);
    auto vTensor = sliceColumnsGpu(pass, qkv, 2 * dim, dim, device);
    auto qDiffTensor = sliceColumnsGpu(pass, qkv, 3 * dim, dim, device);
    auto kDiffTensor = sliceColumnsGpu(pass, qkv, 4 * dim, dim, device);

    auto qNormed =
        dynamicTanhPerHead(pass, qTensor, weights.qNorm, rows, weights.heads, weights.headDim, device);
    auto kNormed =
        dynamicTanhPerHead(pass, kTensor, weights.kNorm, rows, weights.heads, weights.headDim, device);
    auto qDiffNormed = dynamicTanhPerHead(
        pass, qDiffTensor, weights.qNorm, rows, weights.heads, weights.headDim, device);
    auto kDiffNormed = dynamicTanhPerHead(
        pass, kDiffTensor, weights.kNorm, rows, weights.heads, weights.headDim, device);

    auto qRoped = applySegmentRoPE(pass,
                                   qNormed,
                                   weights.invFreq,
                                   weights.heads,
                                   weights.headDim,
                                   band.segmentRows,
                                   device);
    auto kRoped = applySegmentRoPE(pass,
                                   kNormed,
                                   weights.invFreq,
                                   weights.heads,
                                   weights.headDim,
                                   band.segmentRows,
                                   device);
    auto qDiffRoped = applySegmentRoPE(pass,
                                       qDiffNormed,
                                       weights.invFreq,
                                       weights.heads,
                                       weights.headDim,
                                       band.segmentRows,
                                       device);
    auto kDiffRoped = applySegmentRoPE(pass,
                                       kDiffNormed,
                                       weights.invFreq,
                                       weights.heads,
                                       weights.headDim,
                                       band.segmentRows,
                                       device);

    auto primaryOut = bandedAttention(
        pass, qRoped, kRoped, vTensor, weights.heads, weights.headDim, band, device);

    auto diffOut = bandedAttention(pass,
                                   qDiffRoped,
                                   kDiffRoped,
                                   vTensor,
                                   weights.heads,
                                   weights.headDim,
                                   band,
                                   device);

    auto differential = subtractTensorsGpu(pass, primaryOut, diffOut, device);
    auto differentialFlat = reshapeFlat(std::move(differential), {rows, dim});

    auto attnProjected = linear(pass, differentialFlat, weights.toOutWeight, nullptr, device);
    auto afterAttention = addTensorsGpu(pass, input, attnProjected, device);

    auto ffNormed = dynamicTanh(pass,
                               afterAttention,
                               weights.ffNorm.gamma,
                               weights.ffNorm.beta,
                               weights.ffNorm.alpha,
                               device);
    auto ffOutput = weights.useSinusoidalGate
                      ? sinGatedFeedForward(pass,
                                          ffNormed,
                                          weights.ff0Weight,
                                          weights.ff0Bias,
                                          weights.ff2Weight,
                                          weights.ff2Bias,
                                          device)
                      : swiGLU(pass,
                              ffNormed,
                              weights.ff0Weight,
                              weights.ff0Bias,
                              weights.ff2Weight,
                              weights.ff2Bias,
                              device);

    return addTensorsGpu(pass, afterAttention, ffOutput, device);
}

void addCodecTransformerBlockWarmupKernels(KernelWarmup& warmup)
{
    warmup.add<DynamicTanhKernel>();
    warmup.add<SinGateKernel>();
    warmup.add<LinearF32>();
    warmup.add<AddBiasRows>();
    warmup.add<SegmentRoPEKernel>();
    warmup.add<BandedAttentionScoresKernel>();
    warmup.add<BandedAttentionRowStatsKernel>();
    warmup.add<BandedAttentionWeightedSumKernel>();
    warmup.add<SwiGLUGateKernel>();
}
}
