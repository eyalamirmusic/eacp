#include "CodecTransformerBlock.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/BandedAttention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/RoPE.h>
#include <eacp/ML/Kernels/SwiGLU.h>
#include <eacp/ML/Kernels/TensorOps.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class SinGateKernel final : public ComputeProgram
{
public:
    SinGateKernel() { compile(); }

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

    auto& gateKernel = GPU::sharedKernel<SinGateKernel>(device);
    gateKernel.hidden = hidden;
    gateKernel.output = gated;
    gateKernel.dispatch(pass, rows, inner);

    return linear(pass, gated, proj2Weight, &proj2Bias, device);
}
} // namespace

Tensor applyCodecTransformerBlock(ComputePass& pass,
                                  const Tensor& input,
                                  const CodecBlockWeights& weights,
                                  const AttentionBand& band,
                                  Device& device)
{
    auto dim = weights.heads * weights.headDim;
    auto rows = input.rows();

    auto normed = dynamicTanh(pass,
                              input,
                              weights.preNorm.gamma,
                              weights.preNorm.beta,
                              weights.preNorm.alpha,
                              device);
    auto qkv = linear(pass, normed, weights.qkvWeight, nullptr, device);

    auto qTensor = qkv.columns(0 * dim, dim);
    auto kTensor = qkv.columns(1 * dim, dim);
    auto vTensor = qkv.columns(2 * dim, dim);
    auto qDiffTensor = qkv.columns(3 * dim, dim);
    auto kDiffTensor = qkv.columns(4 * dim, dim);

    auto qNormed = dynamicTanhPerHead(pass,
                                      qTensor,
                                      weights.qNorm.gamma,
                                      weights.qNorm.beta,
                                      weights.qNorm.alpha,
                                      weights.headDim,
                                      device);
    auto kNormed = dynamicTanhPerHead(pass,
                                      kTensor,
                                      weights.kNorm.gamma,
                                      weights.kNorm.beta,
                                      weights.kNorm.alpha,
                                      weights.headDim,
                                      device);
    auto qDiffNormed = dynamicTanhPerHead(pass,
                                          qDiffTensor,
                                          weights.qNorm.gamma,
                                          weights.qNorm.beta,
                                          weights.qNorm.alpha,
                                          weights.headDim,
                                          device);
    auto kDiffNormed = dynamicTanhPerHead(pass,
                                          kDiffTensor,
                                          weights.kNorm.gamma,
                                          weights.kNorm.beta,
                                          weights.kNorm.alpha,
                                          weights.headDim,
                                          device);

    auto qRoped = applyRoPE(pass,
                            qNormed,
                            weights.invFreq,
                            weights.heads,
                            weights.headDim,
                            band.segmentRows,
                            device);
    auto kRoped = applyRoPE(pass,
                            kNormed,
                            weights.invFreq,
                            weights.heads,
                            weights.headDim,
                            band.segmentRows,
                            device);
    auto qDiffRoped = applyRoPE(pass,
                                qDiffNormed,
                                weights.invFreq,
                                weights.heads,
                                weights.headDim,
                                band.segmentRows,
                                device);
    auto kDiffRoped = applyRoPE(pass,
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

    auto differential = subtract(pass, primaryOut, diffOut, device);
    auto differentialFlat = reshape(std::move(differential), {rows, dim});

    auto attnProjected =
        linear(pass, differentialFlat, weights.toOutWeight, nullptr, device);
    auto afterAttention = add(pass, input, attnProjected, device);

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

    return add(pass, afterAttention, ffOutput, device);
}
} // namespace eacp::SA3Codec
