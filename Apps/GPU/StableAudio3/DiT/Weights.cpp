#include "Weights.h"

#include "Ops.h"

namespace eacp::SA3DiT
{
using namespace eacp::GPU;
using namespace eacp::ML;

DiTConfig DiTConfig::smallMusic()
{
    return DiTConfig {
        .embedDim = eacp::SA3DiT::embedDim,
        .depth = eacp::SA3DiT::depth,
        .numHeads = eacp::SA3DiT::numHeads,
        .headDim = eacp::SA3DiT::headDim,
        .condTokenDim = eacp::SA3DiT::condTokenDim,
        .ioChannels = eacp::SA3DiT::ioChannels,
        .numMemoryTokens = eacp::SA3DiT::numMemoryTokens,
        .localAddCondDim = eacp::SA3DiT::localAddCondDim,
        .timestepFeaturesDim = eacp::SA3DiT::timestepFeaturesDim,
        .timestepMinFreq = eacp::SA3DiT::timestepMinFreq,
        .timestepMaxFreq = eacp::SA3DiT::timestepMaxFreq,
        .secondsMinVal = eacp::SA3DiT::secondsMinVal,
        .secondsMaxVal = eacp::SA3DiT::secondsMaxVal,
        .rmsNormEpsilon = eacp::SA3DiT::rmsNormEpsilon,
        .qkNormEpsilon = eacp::SA3DiT::qkNormEpsilon,
        .differential = false,
    };
}

DiTConfig DiTConfig::medium()
{
    return DiTConfig {
        .embedDim = 1536,
        .depth = 24,
        .numHeads = 24,
        .headDim = eacp::SA3DiT::headDim,
        .condTokenDim = eacp::SA3DiT::condTokenDim,
        .ioChannels = eacp::SA3DiT::ioChannels,
        .numMemoryTokens = eacp::SA3DiT::numMemoryTokens,
        .localAddCondDim = eacp::SA3DiT::localAddCondDim,
        .timestepFeaturesDim = eacp::SA3DiT::timestepFeaturesDim,
        .timestepMinFreq = eacp::SA3DiT::timestepMinFreq,
        .timestepMaxFreq = eacp::SA3DiT::timestepMaxFreq,
        .secondsMinVal = eacp::SA3DiT::secondsMinVal,
        .secondsMaxVal = eacp::SA3DiT::secondsMaxVal,
        .rmsNormEpsilon = eacp::SA3DiT::rmsNormEpsilon,
        .qkNormEpsilon = eacp::SA3DiT::qkNormEpsilon,
        .differential = true,
    };
}

namespace
{
std::string layerPrefix(int layer)
{
    return "model.model.transformer.layers." + std::to_string(layer) + ".";
}

LayerWeights loadLayer(const SafetensorsFile& file, int layer, Device& device)
{
    auto prefix = layerPrefix(layer);

    return LayerWeights {
        .preNormGamma = file.loadF32(prefix + "pre_norm.gamma", device),
        .selfAttnQNormGamma =
            file.loadF32(prefix + "self_attn.q_norm.gamma", device),
        .selfAttnKNormGamma =
            file.loadF32(prefix + "self_attn.k_norm.gamma", device),
        .selfAttnQKVWeight = file.loadF32(prefix + "self_attn.to_qkv.weight", device),
        .selfAttnOutWeight = file.loadF32(prefix + "self_attn.to_out.weight", device),
        .crossAttendNormGamma =
            file.loadF32(prefix + "cross_attend_norm.gamma", device),
        .crossAttnQNormGamma =
            file.loadF32(prefix + "cross_attn.q_norm.gamma", device),
        .crossAttnKNormGamma =
            file.loadF32(prefix + "cross_attn.k_norm.gamma", device),
        .crossAttnQWeight = file.loadF32(prefix + "cross_attn.to_q.weight", device),
        .crossAttnKVWeight = file.loadF32(prefix + "cross_attn.to_kv.weight", device),
        .crossAttnOutWeight = file.loadF32(prefix + "cross_attn.to_out.weight", device),
        .toLocalEmbed0Weight =
            file.loadF32(prefix + "to_local_embed.0.weight", device),
        .toLocalEmbed0Bias = file.loadF32(prefix + "to_local_embed.0.bias", device),
        .toLocalEmbed2Weight =
            file.loadF32(prefix + "to_local_embed.2.weight", device),
        .toLocalEmbed2Bias = file.loadF32(prefix + "to_local_embed.2.bias", device),
        .ffNormGamma = file.loadF32(prefix + "ff_norm.gamma", device),
        .ff0ProjWeight = file.loadF32(prefix + "ff.ff.0.proj.weight", device),
        .ff0ProjBias = file.loadF32(prefix + "ff.ff.0.proj.bias", device),
        .ff2Weight = file.loadF32(prefix + "ff.ff.2.weight", device),
        .ff2Bias = file.loadF32(prefix + "ff.ff.2.bias", device),
        .toScaleShiftGate = file.loadF32(prefix + "to_scale_shift_gate", device),
    };
}
}

Weights loadWeights(const SafetensorsFile& file, const DiTConfig& config, Device& device)
{
    auto weights = Weights {
        .config = config,
        .preprocessConvWeight = squeezeTrailingUnitDim(
            file.loadF32("model.model.preprocess_conv.weight", device)),
        .postprocessConvWeight = squeezeTrailingUnitDim(
            file.loadF32("model.model.postprocess_conv.weight", device)),
        .toCondEmbed0Weight = file.loadF32("model.model.to_cond_embed.0.weight", device),
        .toCondEmbed2Weight = file.loadF32("model.model.to_cond_embed.2.weight", device),
        .toGlobalEmbed0Weight =
            file.loadF32("model.model.to_global_embed.0.weight", device),
        .toGlobalEmbed2Weight =
            file.loadF32("model.model.to_global_embed.2.weight", device),
        .toTimestepEmbed0Weight =
            file.loadF32("model.model.to_timestep_embed.0.weight", device),
        .toTimestepEmbed0Bias =
            file.loadF32("model.model.to_timestep_embed.0.bias", device),
        .toTimestepEmbed2Weight =
            file.loadF32("model.model.to_timestep_embed.2.weight", device),
        .toTimestepEmbed2Bias =
            file.loadF32("model.model.to_timestep_embed.2.bias", device),
        .secondsEmbedWeight = file.loadF32(
            "conditioner.conditioners.seconds_total.embedder.embedding.1.weight",
            device),
        .secondsEmbedBias = file.loadF32(
            "conditioner.conditioners.seconds_total.embedder.embedding.1.bias",
            device),
        .memoryTokens = file.loadF32("model.model.transformer.memory_tokens", device),
        .projectInWeight =
            file.loadF32("model.model.transformer.project_in.weight", device),
        .projectOutWeight =
            file.loadF32("model.model.transformer.project_out.weight", device),
        .rotaryInvFreq =
            file.loadF32("model.model.transformer.rotary_pos_emb.inv_freq", device),
        .globalCondEmbedder0Weight = file.loadF32(
            "model.model.transformer.global_cond_embedder.0.weight", device),
        .globalCondEmbedder0Bias = file.loadF32(
            "model.model.transformer.global_cond_embedder.0.bias", device),
        .globalCondEmbedder2Weight = file.loadF32(
            "model.model.transformer.global_cond_embedder.2.weight", device),
        .globalCondEmbedder2Bias = file.loadF32(
            "model.model.transformer.global_cond_embedder.2.bias", device),
    };

    weights.layers.reserve((std::size_t) config.depth);

    for (auto layer = 0; layer < config.depth; ++layer)
        weights.layers.push_back(loadLayer(file, layer, device));

    return weights;
}
}
