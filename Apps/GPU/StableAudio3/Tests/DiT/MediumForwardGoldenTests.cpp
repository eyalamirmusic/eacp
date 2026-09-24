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

auto tMediumForwardMatchesGolden =
    test("SA3DiT/mediumFullForwardMatchesPythonReference") = []
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

    constexpr auto latentLength = 8;
    constexpr auto contextLen = 5;
    auto ioChannelsC = config.ioChannels;
    auto condTokenDimC = config.condTokenDim;

    auto dir = std::string(SA3_DIT_GOLDEN_DIR);
    auto latentInput =
        readGoldenFloats(dir + "/medium_latent.bin", latentLength * ioChannelsC);
    auto contextInput = readGoldenFloats(dir + "/medium_cross_ctx_raw.bin",
                                         contextLen * condTokenDimC);
    auto expected =
        readGoldenFloats(dir + "/medium_full_out.bin", latentLength * ioChannelsC);

    auto latentTensor =
        Tensor::fromHostF32(latentInput.data(), {latentLength, ioChannelsC}, device);
    auto contextTensor = Tensor::fromHostF32(
        contextInput.data(), {contextLen, condTokenDimC}, device);

    auto commands = device.makeCommandBuffer();
    auto out = Tensor::uninitializedF32({latentLength, ioChannelsC}, device);

    {
        auto pass = commands.beginCompute();
        out =
            forward(pass, weights, latentTensor, 0.5f, 20.f, contextTensor, device);
    }

    commands.commit();

    auto actual = out.toHostF32();
    auto gap = maxAllcloseGap(actual, expected, 3.0e-2f, 3.0e-2f);
    check(gap <= 0.f);
};
