#pragma once

#include "CodecConfig.h"
#include "PatchedPretransform.h"
#include "SoftNormBottleneck.h"
#include "TransformerResamplingBlock.h"

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <string>

namespace eacp::SA3Codec
{
class SameCodec
{
public:
    static SameCodec loadFromSafetensors(const ML::SafetensorsFile& file,
                                         const CodecConfig& config = CodecConfig::sameS(),
                                         const std::string& prefix = "pretransform.model",
                                         GPU::Device& device = GPU::Device::shared());

    ML::Tensor encode(const StereoWaveform& waveform,
                      GPU::Device& device = GPU::Device::shared()) const;

    StereoWaveform decode(const ML::Tensor& latent,
                          int sampleCount,
                          GPU::Device& device = GPU::Device::shared()) const;

    ResamplingBlockWeights encoderBlock;
    ML::Tensor encoderProjectionWeight;
    ML::Tensor encoderProjectionBias;

    ResamplingBlockWeights decoderBlock;
    ML::Tensor decoderProjectionWeight;
    ML::Tensor decoderProjectionBias;

    SoftNormBottleneckWeights bottleneck;

private:
    SameCodec(ResamplingBlockWeights encoderBlockToUse,
             ML::Tensor encoderProjectionWeightToUse,
             ML::Tensor encoderProjectionBiasToUse,
             ResamplingBlockWeights decoderBlockToUse,
             ML::Tensor decoderProjectionWeightToUse,
             ML::Tensor decoderProjectionBiasToUse,
             SoftNormBottleneckWeights bottleneckToUse);
};
}
