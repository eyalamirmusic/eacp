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

    globalEmbed = addTensors(pass, globalEmbed, timestepEmbed, device);

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
    auto q = sliceColumns(pass, qkv, 0 * embedDimC, embedDimC, device);
    auto k = sliceColumns(pass, qkv, 1 * embedDimC, embedDimC, device);
    auto v = sliceColumns(pass, qkv, 2 * embedDimC, embedDimC, device);

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

    auto mainAttn = attention(pass,
                              qr,
                              kr,
                              v,
                              config.numHeads,
                              config.headDim,
                              nullptr,
                              nullptr,
                              nullptr,
                              config.qkNormEpsilon,
                              device);

    if (!config.differential)
        return mainAttn;

    auto qDiff = sliceColumns(pass, qkv, 3 * embedDimC, embedDimC, device);
    auto kDiff = sliceColumns(pass, qkv, 4 * embedDimC, embedDimC, device);

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

    auto diffAttn = attention(pass,
                              qDiffR,
                              kDiffR,
                              v,
                              config.numHeads,
                              config.headDim,
                              nullptr,
                              nullptr,
                              nullptr,
                              config.qkNormEpsilon,
                              device);

    return subtractTensors(pass, mainAttn, diffAttn, device);
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
        auto keys =
            sliceColumns(pass, keysAndValues, slice * embedDimC, embedDimC, device);
        return rmsNormPerHead(pass,
                              keys,
                              layer.crossAttnKNormGamma,
                              config.headDim,
                              config.qkNormEpsilon,
                              device);
    };

    if (!config.differential)
        return PromptKeys {
            .keys = normedKeys(0),
            .diffKeys = std::nullopt,
            .values =
                sliceColumns(pass, keysAndValues, 1 * embedDimC, embedDimC, device),
        };

    return PromptKeys {
        .keys = normedKeys(0),
        .diffKeys = normedKeys(1),
        .values =
            sliceColumns(pass, keysAndValues, 2 * embedDimC, embedDimC, device),
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
                         prompt.values,
                         config.numHeads,
                         config.headDim,
                         nullptr,
                         nullptr,
                         nullptr,
                         config.qkNormEpsilon,
                         device);
    }

    auto q2Full = linear(pass, xn2, layer.crossAttnQWeight, nullptr, device);
    auto q2 = sliceColumns(pass, q2Full, 0 * embedDimC, embedDimC, device);
    auto q2Diff = sliceColumns(pass, q2Full, 1 * embedDimC, embedDimC, device);

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
                               prompt.values,
                               config.numHeads,
                               config.headDim,
                               nullptr,
                               nullptr,
                               nullptr,
                               config.qkNormEpsilon,
                               device);
    auto diffCross = attention(pass,
                               q2DiffN,
                               *prompt.diffKeys,
                               prompt.values,
                               config.numHeads,
                               config.headDim,
                               nullptr,
                               nullptr,
                               nullptr,
                               config.qkNormEpsilon,
                               device);

    return subtractTensors(pass, mainCross, diffCross, device);
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

    auto modulation = addTensors(pass, globalCondBase, layer.toScaleShiftGate, device);

    auto scaleSelf = sliceColumns(pass, modulation, 0 * embedDimC, embedDimC, device);
    auto shiftSelf = sliceColumns(pass, modulation, 1 * embedDimC, embedDimC, device);
    auto gateSelf = sliceColumns(pass, modulation, 2 * embedDimC, embedDimC, device);
    auto scaleFf = sliceColumns(pass, modulation, 3 * embedDimC, embedDimC, device);
    auto shiftFf = sliceColumns(pass, modulation, 4 * embedDimC, embedDimC, device);
    auto gateFf = sliceColumns(pass, modulation, 5 * embedDimC, embedDimC, device);

    auto xn = rmsNorm(pass, x, layer.preNormGamma, config.rmsNormEpsilon, device);
    auto xm = adaLNModulate(pass, xn, scaleSelf, shiftSelf, device);

    auto attnOut = selfAttentionOutput(pass, config, layer, xm, rotaryInvFreq, device);
    auto attnFlat = reshapeFlat(std::move(attnOut), {seqLen, embedDimC});
    auto attnProj = linear(pass, attnFlat, layer.selfAttnOutWeight, nullptr, device);
    auto gatedSelf = sigmoidGate(pass, attnProj, gateSelf, device);

    auto x1 = addTensors(pass, x, gatedSelf, device);

    auto xn2 = rmsNorm(pass, x1, layer.crossAttendNormGamma, config.rmsNormEpsilon, device);
    auto crossOut = crossAttentionOutput(pass, config, layer, xn2, prompt, device);
    auto crossFlat = reshapeFlat(std::move(crossOut), {seqLen, embedDimC});
    auto crossProj = linear(pass, crossFlat, layer.crossAttnOutWeight, nullptr, device);

    auto x2 = addTensors(pass, x1, crossProj, device);

    auto x3 = applyLocalConditioning
                ? withLocalConditioning(pass, config, layer, std::move(x2), device)
                : std::move(x2);

    auto xn3 = rmsNorm(pass, x3, layer.ffNormGamma, config.rmsNormEpsilon, device);
    auto xm3 = adaLNModulate(pass, xn3, scaleFf, shiftFf, device);
    auto ffOut =
        swiGLU(pass, xm3, layer.ff0ProjWeight, layer.ff0ProjBias, layer.ff2Weight, layer.ff2Bias, device);
    auto gatedFf = sigmoidGate(pass, ffOut, gateFf, device);

    return addTensors(pass, x3, gatedFf, device);
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
    pre = addTensors(pass, pre, latent, device);

    auto x0 = linear(pass, pre, weights.projectInWeight, nullptr, device);

    auto seq = concatRows(pass, weights.memoryTokens, x0, device);

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
    post = addTensors(pass, post, projOut, device);

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
