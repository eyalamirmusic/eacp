#include "SA3DiT.h"

#include "../GPU/CommandBuffer/CommandBuffer.h"
#include "../ML/Kernels/Activation.h"
#include "../ML/Kernels/Attention.h"
#include "../ML/Kernels/Linear.h"
#include "../ML/Kernels/Norm.h"
#include "../ML/Kernels/RoPE.h"
#include "../ML/Kernels/SwiGLU.h"
#include "Ops.h"

#include <cmath>

namespace eacp::SA3DiT
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
Tensor withLocalConditioning(ComputePass& pass,
                             const LayerWeights& layer,
                             Tensor x,
                             Device& device)
{
    auto zeros = std::vector<float>((std::size_t) localAddCondDim, 0.f);
    auto zerosTensor = Tensor::fromHostF32(zeros.data(), {1, localAddCondDim}, device);
    auto localH =
        linear(pass, zerosTensor, layer.toLocalEmbed0Weight, &layer.toLocalEmbed0Bias, device);
    localH = applyActivation(pass, localH, ActivationKind::SiLU, device);
    auto localEmb =
        linear(pass, localH, layer.toLocalEmbed2Weight, &layer.toLocalEmbed2Bias, device);

    return addBroadcastRow(pass, x, localEmb, numMemoryTokens, device);
}

Tensor rmsNormPerHead(ComputePass& pass,
                      const Tensor& input,
                      const Tensor& gamma,
                      int rowCount,
                      int heads,
                      int headDimToUse,
                      float epsilon,
                      Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto kernel = RMSNormKernel {};
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.output = result.buffer();
    kernel.epsilon = epsilon;
    kernel.prepare(device);
    kernel.dispatch(pass, rowCount * heads, headDimToUse);

    return result;
}
}

std::vector<float> expoFourierFeatures(float value, int dim, float minFreq, float maxFreq)
{
    auto halfDim = dim / 2;
    auto result = std::vector<float>((std::size_t) dim);

    auto logMin = std::log(minFreq);
    auto logMax = std::log(maxFreq);

    for (auto i = 0; i < halfDim; ++i)
    {
        auto ramp = halfDim > 1 ? (float) i / (float) (halfDim - 1) : 0.f;
        auto freq = std::exp(ramp * (logMax - logMin) + logMin);
        auto arg = value * freq * 2.f * (float) M_PI;

        result[(std::size_t) i] = std::cos(arg);
        result[(std::size_t) (halfDim + i)] = std::sin(arg);
    }

    return result;
}

Tensor timestepEmbedding(ComputePass& pass,
                         const Weights& weights,
                         float timestep,
                         Device& device)
{
    auto fourier =
        expoFourierFeatures(timestep, timestepFeaturesDim, timestepMinFreq, timestepMaxFreq);
    auto fourierTensor =
        Tensor::fromHostF32(fourier.data(), {1, timestepFeaturesDim}, device);

    auto h = linear(
        pass, fourierTensor, weights.toTimestepEmbed0Weight, &weights.toTimestepEmbed0Bias, device);
    h = applyActivation(pass, h, ActivationKind::SiLU, device);
    h = linear(
        pass, h, weights.toTimestepEmbed2Weight, &weights.toTimestepEmbed2Bias, device);

    return h;
}

Tensor globalConditioning(ComputePass& pass,
                          const Weights& weights,
                          float timestep,
                          float secondsTotal,
                          Device& device)
{
    auto timestepEmbed = timestepEmbedding(pass, weights, timestep, device);

    auto clamped = std::min(std::max(secondsTotal, secondsMinVal), secondsMaxVal);
    auto normalizedSeconds = clamped / secondsMaxVal;

    auto secondsFourier = expoFourierFeatures(
        normalizedSeconds, timestepFeaturesDim, timestepMinFreq, timestepMaxFreq);
    auto secondsFourierTensor =
        Tensor::fromHostF32(secondsFourier.data(), {1, timestepFeaturesDim}, device);

    auto secondsRaw = linear(
        pass, secondsFourierTensor, weights.secondsEmbedWeight, &weights.secondsEmbedBias, device);

    auto globalEmbed = linear(pass, secondsRaw, weights.toGlobalEmbed0Weight, nullptr, device);
    globalEmbed = applyActivation(pass, globalEmbed, ActivationKind::SiLU, device);
    globalEmbed = linear(pass, globalEmbed, weights.toGlobalEmbed2Weight, nullptr, device);

    globalEmbed = addTensors(pass, globalEmbed, timestepEmbed, device);

    auto base =
        linear(pass, globalEmbed, weights.globalCondEmbedder0Weight, &weights.globalCondEmbedder0Bias, device);
    base = applyActivation(pass, base, ActivationKind::SiLU, device);
    base = linear(pass, base, weights.globalCondEmbedder2Weight, &weights.globalCondEmbedder2Bias, device);

    return base;
}

Tensor transformerBlock(ComputePass& pass,
                        const LayerWeights& layer,
                        const Tensor& x,
                        const Tensor& rotaryInvFreq,
                        const Tensor& globalCondBase,
                        const Tensor& crossAttnContext,
                        bool applyLocalConditioning,
                        Device& device)
{
    auto seqLen = x.rows();
    auto contextLen = crossAttnContext.rows();

    auto modulation = addTensors(pass, globalCondBase, layer.toScaleShiftGate, device);

    auto scaleSelf = sliceColumns(pass, modulation, 0 * embedDim, embedDim, device);
    auto shiftSelf = sliceColumns(pass, modulation, 1 * embedDim, embedDim, device);
    auto gateSelf = sliceColumns(pass, modulation, 2 * embedDim, embedDim, device);
    auto scaleFf = sliceColumns(pass, modulation, 3 * embedDim, embedDim, device);
    auto shiftFf = sliceColumns(pass, modulation, 4 * embedDim, embedDim, device);
    auto gateFf = sliceColumns(pass, modulation, 5 * embedDim, embedDim, device);

    auto xn = rmsNorm(pass, x, layer.preNormGamma, rmsNormEpsilon, device);
    auto xm = adaLNModulate(pass, xn, scaleSelf, shiftSelf, device);

    auto qkv = linear(pass, xm, layer.selfAttnQKVWeight, nullptr, device);
    auto q = sliceColumns(pass, qkv, 0 * embedDim, embedDim, device);
    auto k = sliceColumns(pass, qkv, 1 * embedDim, embedDim, device);
    auto v = sliceColumns(pass, qkv, 2 * embedDim, embedDim, device);

    auto qn = rmsNormPerHead(
        pass, q, layer.selfAttnQNormGamma, seqLen, numHeads, headDim, qkNormEpsilon, device);
    auto kn = rmsNormPerHead(
        pass, k, layer.selfAttnKNormGamma, seqLen, numHeads, headDim, qkNormEpsilon, device);

    auto qr = applyRoPE(pass, qn, rotaryInvFreq, numHeads, headDim, device);
    auto kr = applyRoPE(pass, kn, rotaryInvFreq, numHeads, headDim, device);

    auto attnOut = attention(
        pass, qr, kr, v, numHeads, headDim, nullptr, nullptr, nullptr, qkNormEpsilon, device);
    auto attnFlat = reshapeFlat(std::move(attnOut), {seqLen, embedDim});
    auto attnProj = linear(pass, attnFlat, layer.selfAttnOutWeight, nullptr, device);
    auto gatedSelf = sigmoidGate(pass, attnProj, gateSelf, device);

    auto x1 = addTensors(pass, x, gatedSelf, device);

    auto xn2 = rmsNorm(pass, x1, layer.crossAttendNormGamma, rmsNormEpsilon, device);
    auto q2 = linear(pass, xn2, layer.crossAttnQWeight, nullptr, device);
    auto kv2 = linear(pass, crossAttnContext, layer.crossAttnKVWeight, nullptr, device);
    auto k2 = sliceColumns(pass, kv2, 0 * embedDim, embedDim, device);
    auto v2 = sliceColumns(pass, kv2, 1 * embedDim, embedDim, device);

    auto q2n = rmsNormPerHead(
        pass, q2, layer.crossAttnQNormGamma, seqLen, numHeads, headDim, qkNormEpsilon, device);
    auto k2n = rmsNormPerHead(pass,
                              k2,
                              layer.crossAttnKNormGamma,
                              contextLen,
                              numHeads,
                              headDim,
                              qkNormEpsilon,
                              device);

    auto crossOut = attention(
        pass, q2n, k2n, v2, numHeads, headDim, nullptr, nullptr, nullptr, qkNormEpsilon, device);
    auto crossFlat = reshapeFlat(std::move(crossOut), {seqLen, embedDim});
    auto crossProj = linear(pass, crossFlat, layer.crossAttnOutWeight, nullptr, device);

    auto x2 = addTensors(pass, x1, crossProj, device);

    auto x3 = applyLocalConditioning
                ? withLocalConditioning(pass, layer, std::move(x2), device)
                : std::move(x2);

    auto xn3 = rmsNorm(pass, x3, layer.ffNormGamma, rmsNormEpsilon, device);
    auto xm3 = adaLNModulate(pass, xn3, scaleFf, shiftFf, device);
    auto ffOut =
        swiGLU(pass, xm3, layer.ff0ProjWeight, layer.ff0ProjBias, layer.ff2Weight, layer.ff2Bias, device);
    auto gatedFf = sigmoidGate(pass, ffOut, gateFf, device);

    return addTensors(pass, x3, gatedFf, device);
}

Tensor forward(ComputePass& pass,
              const Weights& weights,
              const Tensor& latent,
              float timestep,
              float secondsTotal,
              const Tensor& crossAttnContext,
              Device& device)
{
    auto latentLength = latent.rows();

    auto pre = linear(pass, latent, weights.preprocessConvWeight, nullptr, device);
    pre = addTensors(pass, pre, latent, device);

    auto x0 = linear(pass, pre, weights.projectInWeight, nullptr, device);

    auto seq = concatRows(pass, weights.memoryTokens, x0, device);

    auto globalCondBase = globalConditioning(pass, weights, timestep, secondsTotal, device);

    auto projectedContext = linear(pass, crossAttnContext, weights.toCondEmbed0Weight, nullptr, device);
    projectedContext = applyActivation(pass, projectedContext, ActivationKind::SiLU, device);
    projectedContext = linear(pass, projectedContext, weights.toCondEmbed2Weight, nullptr, device);

    for (auto& layer: weights.layers)
        seq = transformerBlock(pass,
                               layer,
                               seq,
                               weights.rotaryInvFreq,
                               globalCondBase,
                               projectedContext,
                               false,
                               device);

    auto latentOut = sliceRows(pass, seq, numMemoryTokens, latentLength, device);
    auto projOut = linear(pass, latentOut, weights.projectOutWeight, nullptr, device);
    auto post = linear(pass, projOut, weights.postprocessConvWeight, nullptr, device);
    post = addTensors(pass, post, projOut, device);

    return post;
}
}
