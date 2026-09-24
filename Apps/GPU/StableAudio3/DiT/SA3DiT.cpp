#include "SA3DiT.h"

#include "Ops.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/ML/Kernels/Activation.h>
#include <eacp/ML/Kernels/Attention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/RoPE.h>
#include <eacp/ML/Kernels/SwiGLU.h>
#include <eacp/ML/Kernels/TensorOps.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace eacp::SA3DiT
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
Tensor withLocalConditioning(ComputePass& pass,
                             const DiTConfig& config,
                             const LayerWeights& layer,
                             Tensor x,
                             Device& device)
{
    auto zeros = std::vector<float>((std::size_t) config.localAddCondDim, 0.f);
    auto zerosTensor =
        Tensor::fromHostF32(zeros.data(), {1, config.localAddCondDim}, device);
    auto localH =
        linear(pass, zerosTensor, layer.toLocalEmbed0Weight, &layer.toLocalEmbed0Bias, device);
    localH = applyActivation(pass, localH, ActivationKind::SiLU, device);
    auto localEmb =
        linear(pass, localH, layer.toLocalEmbed2Weight, &layer.toLocalEmbed2Bias, device);

    return addBroadcastRow(pass, x, localEmb, config.numMemoryTokens, device);
}

}

Tensor timestepEmbedding(ComputePass& pass,
                         const Weights& weights,
                         float timestep,
                         Device& device)
{
    auto& config = weights.config;

    auto fourierTensor = expoFourierFeatures(
        pass, timestep, config.timestepFeaturesDim, config.timestepMinFreq, config.timestepMaxFreq, device);

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
    auto& config = weights.config;

    auto timestepEmbed = timestepEmbedding(pass, weights, timestep, device);

    auto clamped = std::min(std::max(secondsTotal, config.secondsMinVal), config.secondsMaxVal);
    auto normalizedSeconds = clamped / config.secondsMaxVal;

    auto secondsFourierTensor = expoFourierFeatures(
        pass, normalizedSeconds, config.timestepFeaturesDim, config.timestepMinFreq, config.timestepMaxFreq, device);

    auto secondsRaw = linear(
        pass, secondsFourierTensor, weights.secondsEmbedWeight, &weights.secondsEmbedBias, device);

    auto globalEmbed = linear(pass, secondsRaw, weights.toGlobalEmbed0Weight, nullptr, device);
    globalEmbed = applyActivation(pass, globalEmbed, ActivationKind::SiLU, device);
    globalEmbed = linear(pass, globalEmbed, weights.toGlobalEmbed2Weight, nullptr, device);

    globalEmbed = add(pass, globalEmbed, timestepEmbed, device);

    auto base =
        linear(pass, globalEmbed, weights.globalCondEmbedder0Weight, &weights.globalCondEmbedder0Bias, device);
    base = applyActivation(pass, base, ActivationKind::SiLU, device);
    base = linear(pass, base, weights.globalCondEmbedder2Weight, &weights.globalCondEmbedder2Bias, device);

    return base;
}

namespace
{
Tensor selfAttentionOutput(ComputePass& pass,
                          const DiTConfig& config,
                          const LayerWeights& layer,
                          const Tensor& xm,
                          const Tensor& rotaryInvFreq,
                          Device& device)
{
    auto embedDimC = config.embedDim;
    auto qkv = linear(pass, xm, layer.selfAttnQKVWeight, nullptr, device);
    auto q = qkv.columns(0 * embedDimC, embedDimC);
    auto k = qkv.columns(1 * embedDimC, embedDimC);
    auto v = qkv.columns(2 * embedDimC, embedDimC);

    auto qn = rmsNormPerHead(pass,
                             q,
                             layer.selfAttnQNormGamma,
                             config.headDim,
                             config.qkNormEpsilon,
                             device);
    auto kn = rmsNormPerHead(pass,
                             k,
                             layer.selfAttnKNormGamma,
                             config.headDim,
                             config.qkNormEpsilon,
                             device);

    auto qr = applyRoPE(pass, qn, rotaryInvFreq, config.numHeads, config.headDim, device);
    auto kr = applyRoPE(pass, kn, rotaryInvFreq, config.numHeads, config.headDim, device);

    auto mainAttn =
        attention(pass, qr, kr, v, config.numHeads, config.headDim, {}, device);

    if (!config.differential)
        return mainAttn;

    auto qDiff = qkv.columns(3 * embedDimC, embedDimC);
    auto kDiff = qkv.columns(4 * embedDimC, embedDimC);

    auto qDiffN = rmsNormPerHead(pass,
                                 qDiff,
                                 layer.selfAttnQNormGamma,
                                 config.headDim,
                                 config.qkNormEpsilon,
                                 device);
    auto kDiffN = rmsNormPerHead(pass,
                                 kDiff,
                                 layer.selfAttnKNormGamma,
                                 config.headDim,
                                 config.qkNormEpsilon,
                                 device);

    auto qDiffR = applyRoPE(pass, qDiffN, rotaryInvFreq, config.numHeads, config.headDim, device);
    auto kDiffR = applyRoPE(pass, kDiffN, rotaryInvFreq, config.numHeads, config.headDim, device);

    auto diffAttn = attention(
        pass, qDiffR, kDiffR, v, config.numHeads, config.headDim, {}, device);

    return subtract(pass, mainAttn, diffAttn, device);
}

PromptKeys promptKeysFor(ComputePass& pass,
                         const DiTConfig& config,
                         const LayerWeights& layer,
                         const Tensor& projectedContext,
                         Device& device)
{
    auto embedDimC = config.embedDim;
    auto keysAndValues =
        linear(pass, projectedContext, layer.crossAttnKVWeight, nullptr, device);

    auto normedKeys = [&](int slice)
    {
        return rmsNormPerHead(pass,
                              keysAndValues.columns(slice * embedDimC, embedDimC),
                              layer.crossAttnKNormGamma,
                              config.headDim,
                              config.qkNormEpsilon,
                              device);
    };

    if (!config.differential)
        return PromptKeys {
            .keys = normedKeys(0),
            .diffKeys = std::nullopt,
            .keysAndValues = std::move(keysAndValues),
            .valueColumn = 1 * embedDimC,
        };

    return PromptKeys {
        .keys = normedKeys(0),
        .diffKeys = normedKeys(1),
        .keysAndValues = std::move(keysAndValues),
        .valueColumn = 2 * embedDimC,
    };
}

Tensor projectContext(ComputePass& pass,
                      const Weights& weights,
                      const Tensor& crossAttnContext,
                      Device& device)
{
    auto projected =
        linear(pass, crossAttnContext, weights.toCondEmbed0Weight, nullptr, device);
    projected = applyActivation(pass, projected, ActivationKind::SiLU, device);
    return linear(pass, projected, weights.toCondEmbed2Weight, nullptr, device);
}

Tensor crossAttentionOutput(ComputePass& pass,
                            const DiTConfig& config,
                            const LayerWeights& layer,
                            const Tensor& xn2,
                            const PromptKeys& prompt,
                            Device& device)
{
    auto embedDimC = config.embedDim;

    if (!config.differential)
    {
        auto q2 = linear(pass, xn2, layer.crossAttnQWeight, nullptr, device);
        auto q2n = rmsNormPerHead(pass,
                                  q2,
                                  layer.crossAttnQNormGamma,
                                  config.headDim,
                                  config.qkNormEpsilon,
                                  device);

        return attention(pass,
                         q2n,
                         prompt.keys,
                         prompt.values(),
                         config.numHeads,
                         config.headDim,
                         {},
                         device);
    }

    auto q2Full = linear(pass, xn2, layer.crossAttnQWeight, nullptr, device);
    auto q2 = q2Full.columns(0 * embedDimC, embedDimC);
    auto q2Diff = q2Full.columns(1 * embedDimC, embedDimC);

    auto q2n = rmsNormPerHead(pass,
                              q2,
                              layer.crossAttnQNormGamma,
                              config.headDim,
                              config.qkNormEpsilon,
                              device);
    auto q2DiffN = rmsNormPerHead(pass,
                                  q2Diff,
                                  layer.crossAttnQNormGamma,
                                  config.headDim,
                                  config.qkNormEpsilon,
                                  device);

    auto mainCross = attention(pass,
                               q2n,
                               prompt.keys,
                               prompt.values(),
                               config.numHeads,
                               config.headDim,
                               {},
                               device);
    auto diffCross = attention(pass,
                               q2DiffN,
                               *prompt.diffKeys,
                               prompt.values(),
                               config.numHeads,
                               config.headDim,
                               {},
                               device);

    return subtract(pass, mainCross, diffCross, device);
}
}

Tensor transformerBlock(ComputePass& pass,
                        const DiTConfig& config,
                        const LayerWeights& layer,
                        const Tensor& x,
                        const Tensor& rotaryInvFreq,
                        const Tensor& globalCondBase,
                        const Tensor& crossAttnContext,
                        bool applyLocalConditioning,
                        Device& device)
{
    auto prompt = promptKeysFor(pass, config, layer, crossAttnContext, device);

    return transformerBlock(pass,
                            config,
                            layer,
                            x,
                            rotaryInvFreq,
                            globalCondBase,
                            prompt,
                            applyLocalConditioning,
                            device);
}

Tensor transformerBlock(ComputePass& pass,
                        const DiTConfig& config,
                        const LayerWeights& layer,
                        const Tensor& x,
                        const Tensor& rotaryInvFreq,
                        const Tensor& globalCondBase,
                        const PromptKeys& prompt,
                        bool applyLocalConditioning,
                        Device& device)
{
    auto embedDimC = config.embedDim;
    auto seqLen = x.rows();

    auto modulation = add(pass, globalCondBase, layer.toScaleShiftGate, device);

    // The modulation is one row of six: each part is read where it lies.
    auto modulationPart = [&](int part)
    {
        auto bytes = (std::int64_t) embedDimC * (std::int64_t) sizeof(float);
        auto whole = modulation.range();
        return BufferRange {whole.buffer, whole.offset + part * bytes, bytes};
    };

    auto scaleSelf = modulationPart(0);
    auto shiftSelf = modulationPart(1);
    auto gateSelf = modulationPart(2);
    auto scaleFf = modulationPart(3);
    auto shiftFf = modulationPart(4);
    auto gateFf = modulationPart(5);

    auto xn = rmsNorm(pass, x, layer.preNormGamma, config.rmsNormEpsilon, device);
    auto xm = adaLNModulate(pass, xn, scaleSelf, shiftSelf, device);

    auto attnOut = selfAttentionOutput(pass, config, layer, xm, rotaryInvFreq, device);
    auto attnFlat = reshape(std::move(attnOut), {seqLen, embedDimC});
    auto attnProj = linear(pass, attnFlat, layer.selfAttnOutWeight, nullptr, device);
    auto gatedSelf = sigmoidGate(pass, attnProj, gateSelf, device);

    auto x1 = add(pass, x, gatedSelf, device);

    auto xn2 = rmsNorm(pass, x1, layer.crossAttendNormGamma, config.rmsNormEpsilon, device);
    auto crossOut = crossAttentionOutput(pass, config, layer, xn2, prompt, device);
    auto crossFlat = reshape(std::move(crossOut), {seqLen, embedDimC});
    auto crossProj = linear(pass, crossFlat, layer.crossAttnOutWeight, nullptr, device);

    auto x2 = add(pass, x1, crossProj, device);

    auto x3 = applyLocalConditioning
                ? withLocalConditioning(pass, config, layer, std::move(x2), device)
                : std::move(x2);

    auto xn3 = rmsNorm(pass, x3, layer.ffNormGamma, config.rmsNormEpsilon, device);
    auto xm3 = adaLNModulate(pass, xn3, scaleFf, shiftFf, device);
    auto ffOut =
        swiGLU(pass, xm3, layer.ff0ProjWeight, layer.ff0ProjBias, layer.ff2Weight, layer.ff2Bias, device);
    auto gatedFf = sigmoidGate(pass, ffOut, gateFf, device);

    return add(pass, x3, gatedFf, device);
}

Prompt preparePrompt(ComputePass& pass,
                     const Weights& weights,
                     const Tensor& crossAttnContext,
                     Device& device)
{
    auto projected = projectContext(pass, weights, crossAttnContext, device);
    auto prompt = Prompt {};
    prompt.layers.reserve(weights.layers.size());

    for (const auto& layer: weights.layers)
        prompt.layers.push_back(
            promptKeysFor(pass, weights.config, layer, projected, device));

    return prompt;
}

Prompt preparePrompt(const Weights& weights,
                     const Tensor& crossAttnContext,
                     Device& device)
{
    auto prompt = std::optional<Prompt> {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        prompt = preparePrompt(pass, weights, crossAttnContext, device);
    }

    commands.commit();
    return std::move(*prompt);
}

Tensor forward(ComputePass& pass,
               const Weights& weights,
               const Tensor& latent,
               float timestep,
               float secondsTotal,
               const Prompt& prompt,
               Device& device)
{
    auto& config = weights.config;
    auto latentLength = latent.rows();

    auto pre = linear(pass, latent, weights.preprocessConvWeight, nullptr, device);
    pre = add(pass, pre, latent, device);

    auto x0 = linear(pass, pre, weights.projectInWeight, nullptr, device);

    auto seq = concatRows(pass, {weights.memoryTokens, x0}, device);

    auto globalCondBase = globalConditioning(pass, weights, timestep, secondsTotal, device);

    for (auto layer = std::size_t {0}; layer < weights.layers.size(); ++layer)
        seq = transformerBlock(pass,
                               config,
                               weights.layers[layer],
                               seq,
                               weights.rotaryInvFreq,
                               globalCondBase,
                               prompt.layers[layer],
                               false,
                               device);

    auto latentOut = sliceRows(pass, seq, config.numMemoryTokens, latentLength, device);
    auto projOut = linear(pass, latentOut, weights.projectOutWeight, nullptr, device);
    auto post = linear(pass, projOut, weights.postprocessConvWeight, nullptr, device);
    post = add(pass, post, projOut, device);

    return post;
}

Tensor forward(ComputePass& pass,
               const Weights& weights,
               const Tensor& latent,
               float timestep,
               float secondsTotal,
               const Tensor& crossAttnContext,
               Device& device)
{
    auto prompt = preparePrompt(pass, weights, crossAttnContext, device);
    return forward(pass, weights, latent, timestep, secondsTotal, prompt, device);
}
}
