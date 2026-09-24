#include <NanoTest/NanoTest.h>
#include <Tests/SkipWithoutCheckpoint.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <DiT/SA3DiT.h>
#include <DiT/Weights.h>

#include "GoldenIO.h"
#include <Checkpoints.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;
using namespace eacp::SA3DiT::TestSupport;

auto tMediumSingleLayerMatchesGolden =
    test("SA3DiT/mediumSingleLayerMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    if (SA3Checkpoints::skipWithoutCheckpoint(SA3Checkpoints::medium,
                                              "model.safetensors"))
        return;

    auto file = SafetensorsFile::open(
        SA3Checkpoints::directory(SA3Checkpoints::medium) / "model.safetensors");

    if (!file.has_value())
        return;

    auto config = DiTConfig::medium();
    auto weights = loadWeights(*file, config, device);

    check(config.differential);

    constexpr auto seqLen = 72;
    constexpr auto contextLen = 5;
    auto embedDim = config.embedDim;

    auto dir = std::string(SA3_DIT_GOLDEN_DIR);
    auto seqInput =
        readGoldenFloats(dir + "/medium_layer0_input_seq.bin", seqLen * embedDim);
    auto contextInput =
        readGoldenFloats(dir + "/medium_cross_ctx_proj.bin", contextLen * embedDim);
    auto globalCondInput =
        readGoldenFloats(dir + "/medium_global_cond_base.bin", embedDim * 6);
    auto expected =
        readGoldenFloats(dir + "/medium_layer0_out.bin", seqLen * embedDim);

    auto seqTensor =
        Tensor::fromHostF32(seqInput.data(), {seqLen, embedDim}, device);
    auto contextTensor =
        Tensor::fromHostF32(contextInput.data(), {contextLen, embedDim}, device);
    auto globalCondTensor =
        Tensor::fromHostF32(globalCondInput.data(), {1, embedDim * 6}, device);

    auto commands = device.makeCommandBuffer();
    auto out = Tensor::uninitializedF32({seqLen, embedDim}, device);

    {
        auto pass = commands.beginCompute();
        out = transformerBlock(pass,
                               config,
                               weights.layers[0],
                               seqTensor,
                               weights.rotaryInvFreq,
                               globalCondTensor,
                               contextTensor,
                               false,
                               device);
    }

    commands.commit();

    auto actual = out.toHostF32();
    auto gap = maxAllcloseGap(actual, expected, 1.0e-2f, 1.0e-2f);
    check(gap <= 0.f);
};
