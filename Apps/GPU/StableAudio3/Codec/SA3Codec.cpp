#include "SA3Codec.h"

#include "GpuOps.h"
#include "HostMatrix.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/TensorOps.h>

#include <optional>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
DynamicTanhWeights loadDynamicTanh(const SafetensorsFile& file,
                                   const std::string& prefix,
                                   Device& device)
{
    return DynamicTanhWeights {.gamma = file.loadF32(prefix + ".gamma", device),
                               .beta = file.loadF32(prefix + ".beta", device),
                               .alpha = file.loadScalar(prefix + ".alpha")};
}

CodecBlockWeights loadCodecBlockWeights(const SafetensorsFile& file,
                                        const std::string& prefix,
                                        int heads,
                                        int headDim,
                                        bool useSinusoidalGate,
                                        Device& device)
{
    return CodecBlockWeights {
        .preNorm = loadDynamicTanh(file, prefix + ".pre_norm", device),
        .qkvWeight = file.loadF32(prefix + ".self_attn.to_qkv.weight", device),
        .toOutWeight = file.loadF32(prefix + ".self_attn.to_out.weight", device),
        .qNorm = loadDynamicTanh(file, prefix + ".self_attn.q_norm", device),
        .kNorm = loadDynamicTanh(file, prefix + ".self_attn.k_norm", device),
        .invFreq = file.loadF32(prefix + ".rope.inv_freq", device),
        .ffNorm = loadDynamicTanh(file, prefix + ".ff_norm", device),
        .ff0Weight = file.loadF32(prefix + ".ff.ff.0.proj.weight", device),
        .ff0Bias = file.loadF32(prefix + ".ff.ff.0.proj.bias", device),
        .ff2Weight = file.loadF32(prefix + ".ff.ff.2.weight", device),
        .ff2Bias = file.loadF32(prefix + ".ff.ff.2.bias", device),
        .heads = heads,
        .headDim = headDim,
        .useSinusoidalGate = useSinusoidalGate,
    };
}

ResamplingBlockWeights loadResamplingBlock(const SafetensorsFile& file,
                                           const std::string& prefix,
                                           bool isEncoder,
                                           int inChannels,
                                           int outChannels,
                                           bool mappingConvKernelThree,
                                           const CodecConfig& config,
                                           Device& device)
{
    auto layers = std::vector<CodecBlockWeights> {};
    layers.reserve((std::size_t) config.transformerDepth);

    for (auto i = 0; i < config.transformerDepth; ++i)
    {
        auto useSinusoidalGate = !isEncoder
                                   && (config.transformerDepth - i) < config.decoderSinusoidalBlockCount;

        layers.push_back(loadCodecBlockWeights(file,
                                              prefix + ".transformers." + std::to_string(i),
                                              config.heads(),
                                              config.dimHeads,
                                              useSinusoidalGate,
                                              device));
    }

    return ResamplingBlockWeights {
        .isEncoder = isEncoder,
        .inChannels = inChannels,
        .outChannels = outChannels,
        .stride = config.stride,
        .chunkSize = config.chunkSize,
        .transformerDepth = config.transformerDepth,
        .attentionMode = config.attentionMode,
        .slidingWindowRadiusChunks = config.slidingWindowRadiusChunks,
        .mapping = loadWNConv1d(file,
                                prefix + ".mapping",
                                inChannels,
                                outChannels,
                                mappingConvKernelThree ? 3 : 1,
                                true,
                                device),
        .newTokens = reshape(file.loadF32(prefix + ".new_tokens", device),
                             {config.transformerDim}),
        .layers = std::move(layers),
    };
}
}

SameCodec::SameCodec(ResamplingBlockWeights encoderBlockToUse,
                     Tensor encoderProjectionWeightToUse,
                     Tensor encoderProjectionBiasToUse,
                     ResamplingBlockWeights decoderBlockToUse,
                     Tensor decoderProjectionWeightToUse,
                     Tensor decoderProjectionBiasToUse,
                     SoftNormBottleneckWeights bottleneckToUse)
    : encoderBlock(std::move(encoderBlockToUse))
    , encoderProjectionWeight(std::move(encoderProjectionWeightToUse))
    , encoderProjectionBias(std::move(encoderProjectionBiasToUse))
    , decoderBlock(std::move(decoderBlockToUse))
    , decoderProjectionWeight(std::move(decoderProjectionWeightToUse))
    , decoderProjectionBias(std::move(decoderProjectionBiasToUse))
    , bottleneck(std::move(bottleneckToUse))
{
}

namespace
{
std::string joinedName(const std::string& prefix, const std::string& suffix)
{
    return prefix.empty() ? suffix : prefix + "." + suffix;
}

ResamplingBlockWeights loadDecoderBlock(const SafetensorsFile& file,
                                        const CodecConfig& config,
                                        const std::string& prefix,
                                        Device& device)
{
    return loadResamplingBlock(file,
                               joinedName(prefix, "decoder.layers.3"),
                               false,
                               config.transformerDim,
                               config.patchChannels,
                               config.decoderMappingKernelSize == 3,
                               config,
                               device);
}

SoftNormBottleneckWeights loadBottleneck(const SafetensorsFile& file,
                                         const std::string& prefix,
                                         Device& device)
{
    return SoftNormBottleneckWeights {
        .scalingFactor =
            file.loadF32(joinedName(prefix, "bottleneck.scaling_factor"), device),
        .bias = file.loadF32(joinedName(prefix, "bottleneck.bias"), device),
        .runningStd = file.loadScalar(joinedName(prefix, "bottleneck.running_std")),
    };
}

StereoWaveform decodeLatent(const Tensor& latent,
                            int sampleCount,
                            const ResamplingBlockWeights& decoderBlock,
                            const Tensor& decoderProjectionWeight,
                            const Tensor& decoderProjectionBias,
                            const SoftNormBottleneckWeights& bottleneck,
                            Device& device)
{
    auto projected = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto denormalized =
            softNormBottleneckDecode(pass, latent, bottleneck, device);
        projected = linear(pass,
                           denormalized,
                           decoderProjectionWeight,
                           &decoderProjectionBias,
                           device);
    }
    commands.commit();

    auto waveformTensor =
        applyTransformerResamplingBlock(*projected, decoderBlock, device);

    auto waveformHost = HostMatrix {
        waveformTensor.toHostF32(), waveformTensor.rows(), waveformTensor.cols()};

    return patchedPretransformDecode(waveformHost, sampleCount);
}
} // namespace

SameCodec SameCodec::loadFromSafetensors(const SafetensorsFile& file,
                                         const CodecConfig& config,
                                         const std::string& prefix,
                                         Device& device)
{
    auto join = [&prefix](const std::string& suffix)
    { return joinedName(prefix, suffix); };

    auto encoderBlock = loadResamplingBlock(file,
                                            join("encoder.layers.0"),
                                            true,
                                            config.patchChannels,
                                            config.transformerDim,
                                            false,
                                            config,
                                            device);

    auto decoderBlock = loadDecoderBlock(file, config, prefix, device);
    auto bottleneck = loadBottleneck(file, prefix, device);

    return SameCodec {std::move(encoderBlock),
                      file.loadF32(join("encoder.layers.2.weight"), device),
                      file.loadF32(join("encoder.layers.2.bias"), device),
                      std::move(decoderBlock),
                      file.loadF32(join("decoder.layers.1.weight"), device),
                      file.loadF32(join("decoder.layers.1.bias"), device),
                      std::move(bottleneck)};
}

Tensor SameCodec::encode(const StereoWaveform& waveform, Device& device) const
{
    auto patched = patchedPretransformEncode(waveform);
    auto patchedTensor =
        Tensor::fromHostF32(patched.data.data(), {patched.rows, patched.cols}, device);

    auto folded = applyTransformerResamplingBlock(patchedTensor, encoderBlock, device);

    auto result = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto projected =
            linear(pass, folded, encoderProjectionWeight, &encoderProjectionBias, device);

        result = softNormBottleneckEncode(pass, projected, bottleneck, device);
    }
    commands.commit();

    return std::move(*result);
}

StereoWaveform SameCodec::decode(const Tensor& latent, int sampleCount, Device& device) const
{
    return decodeLatent(latent,
                        sampleCount,
                        decoderBlock,
                        decoderProjectionWeight,
                        decoderProjectionBias,
                        bottleneck,
                        device);
}

SameDecoder SameDecoder::loadFromSafetensors(const SafetensorsFile& file,
                                             const CodecConfig& config,
                                             const std::string& prefix,
                                             Device& device)
{
    auto join = [&prefix](const std::string& suffix)
    { return joinedName(prefix, suffix); };

    return SameDecoder {
        .decoderBlock = loadDecoderBlock(file, config, prefix, device),
        .decoderProjectionWeight =
            file.loadF32(join("decoder.layers.1.weight"), device),
        .decoderProjectionBias = file.loadF32(join("decoder.layers.1.bias"), device),
        .bottleneck = loadBottleneck(file, prefix, device),
    };
}

StereoWaveform
    SameDecoder::decode(const Tensor& latent, int sampleCount, Device& device) const
{
    return decodeLatent(latent,
                        sampleCount,
                        decoderBlock,
                        decoderProjectionWeight,
                        decoderProjectionBias,
                        bottleneck,
                        device);
}
}
