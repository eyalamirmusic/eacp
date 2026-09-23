#include "SA3Codec.h"

#include "HostMatrix.h"

#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>

#include <optional>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
constexpr auto codecHeads = 12;
constexpr auto codecHeadDim = 64;
constexpr auto codecTransformerDim = codecHeads * codecHeadDim;
constexpr auto codecTransformerDepth = 6;
constexpr auto codecChunkSize = 32;
constexpr auto codecStride = 16;
constexpr auto codecPatchChannels = 512;

std::vector<float> readFloats(const SafetensorsFile& file, const std::string& name, int count)
{
    auto pointer = reinterpret_cast<const float*>(file.rawBytes(name));
    return std::vector<float>(pointer, pointer + count);
}

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
        .heads = codecHeads,
        .headDim = codecHeadDim,
    };
}

ResamplingBlockWeights loadResamplingBlock(const SafetensorsFile& file,
                                           const std::string& prefix,
                                           bool isEncoder,
                                           int inChannels,
                                           int outChannels,
                                           bool mappingConvKernelThree,
                                           Device& device)
{
    auto layers = std::vector<CodecBlockWeights> {};
    layers.reserve(codecTransformerDepth);

    for (auto i = 0; i < codecTransformerDepth; ++i)
        layers.push_back(loadCodecBlockWeights(
            file, prefix + ".transformers." + std::to_string(i), device));

    auto newTokens = readFloats(file, prefix + ".new_tokens", codecTransformerDim);

    return ResamplingBlockWeights {
        .isEncoder = isEncoder,
        .inChannels = inChannels,
        .outChannels = outChannels,
        .stride = codecStride,
        .chunkSize = codecChunkSize,
        .transformerDepth = codecTransformerDepth,
        .mapping = loadWNConv1d(file,
                                prefix + ".mapping",
                                inChannels,
                                outChannels,
                                mappingConvKernelThree ? 3 : 1,
                                true,
                                device),
        .newTokens = Tensor::fromHostF32(newTokens.data(), {codecTransformerDim}, device),
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

SameCodec SameCodec::loadFromSafetensors(const SafetensorsFile& file,
                                         const std::string& prefix,
                                         Device& device)
{
    auto encoderBlock = loadResamplingBlock(file,
                                            prefix + ".encoder.layers.0",
                                            true,
                                            codecPatchChannels,
                                            codecTransformerDim,
                                            false,
                                            device);

    auto decoderBlock = loadResamplingBlock(file,
                                            prefix + ".decoder.layers.3",
                                            false,
                                            codecTransformerDim,
                                            codecPatchChannels,
                                            true,
                                            device);

    auto bottleneck = SoftNormBottleneckWeights {
        .scalingFactor = file.loadF32(prefix + ".bottleneck.scaling_factor", device),
        .bias = file.loadF32(prefix + ".bottleneck.bias", device),
        .runningStd = file.loadScalar(prefix + ".bottleneck.running_std"),
    };

    return SameCodec {std::move(encoderBlock),
                      file.loadF32(prefix + ".encoder.layers.2.weight", device),
                      file.loadF32(prefix + ".encoder.layers.2.bias", device),
                      std::move(decoderBlock),
                      file.loadF32(prefix + ".decoder.layers.1.weight", device),
                      file.loadF32(prefix + ".decoder.layers.1.bias", device),
                      std::move(bottleneck)};
}

Tensor SameCodec::encode(const StereoWaveform& waveform, Device& device) const
{
    auto patched = patchedPretransformEncode(waveform);
    auto patchedTensor =
        Tensor::fromHostF32(patched.data.data(), {patched.rows, patched.cols}, device);

    auto result = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto folded = applyTransformerResamplingBlock(pass, patchedTensor, encoderBlock, device);
        auto projected =
            linear(pass, folded, encoderProjectionWeight, &encoderProjectionBias, device);

        result = softNormBottleneckEncode(pass, projected, bottleneck, device);
    }
    commands.commit();

    return std::move(*result);
}

StereoWaveform SameCodec::decode(const Tensor& latent, int sampleCount, Device& device) const
{
    auto waveformTensor = std::optional<Tensor> {};

    auto commands = device.makeCommandBuffer();
    {
        auto pass = commands.beginCompute();

        auto denormalized = softNormBottleneckDecode(pass, latent, bottleneck, device);
        auto projected =
            linear(pass, denormalized, decoderProjectionWeight, &decoderProjectionBias, device);

        waveformTensor = applyTransformerResamplingBlock(pass, projected, decoderBlock, device);
    }
    commands.commit();

    auto waveformHost =
        HostMatrix {waveformTensor->toHostF32(), waveformTensor->rows(), waveformTensor->cols()};

    return patchedPretransformDecode(waveformHost, sampleCount);
}
}
