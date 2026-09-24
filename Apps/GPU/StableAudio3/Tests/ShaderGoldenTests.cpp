#include <ShaderGolden.h>

#include <Codec/CodecTransformerBlock.h>
#include <Codec/GpuOps.h>
#include <Codec/SoftNormBottleneck.h>
#include <DiT/Ops.h>
#include <TextEncoder/Encoder/GemmaAttention.h>
#include <TextEncoder/SA3TextEncoder.h>

// Every kernel the app builds beside the ML library's, which the library corpus
// in Tests/GPU covers. A kernel added to the codec, the DiT or the text encoder
// gets a line here.

using namespace eacp;
using namespace eacp::ShaderGolden;

namespace
{
std::vector<Entry> appShaders()
{
    return {
        {"SA3Codec/FoldWithNewTokensKernel",
         walked(SA3Codec::forEachGpuOpsShaderGraph, 0)},
        {"SA3Codec/UnfoldLastSegmentKernel",
         walked(SA3Codec::forEachGpuOpsShaderGraph, 1)},
        {"SA3Codec/Conv1dUnfoldKernel",
         walked(SA3Codec::forEachGpuOpsShaderGraph, 2)},
        {"SA3Codec/SinGateKernel",
         walked(SA3Codec::forEachTransformerBlockShaderGraph, 0)},
        {"SA3Codec/SoftNormEncodeKernel",
         walked(SA3Codec::forEachBottleneckShaderGraph, 0)},
        {"SA3Codec/SoftNormDecodeKernel",
         walked(SA3Codec::forEachBottleneckShaderGraph, 1)},
        {"SA3DiT/AddBroadcastRowKernel", program<SA3DiT::AddBroadcastRowKernel>()},
        {"SA3DiT/AdaLNModulateKernel", program<SA3DiT::AdaLNModulateKernel>()},
        {"SA3DiT/SigmoidGateKernel", program<SA3DiT::SigmoidGateKernel>()},
        {"SA3DiT/ExpoFourierFeaturesKernel",
         program<SA3DiT::ExpoFourierFeaturesKernel>()},
        {"SA3TextEncoder/GemmaAttentionScoresKernel",
         program<SA3TextEncoder::GemmaAttentionScoresKernel>()},
        {"SA3TextEncoder/PadRowSelectKernel",
         walked(SA3TextEncoder::forEachTextEncoderShaderGraph, 0)},
    };
}

const auto registered =
    registerCorpus("SA3ShaderGolden", SA3_SHADER_GOLDEN_DIR, appShaders());
} // namespace
