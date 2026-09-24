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

auto tMediumTimestepEmbeddingMatchesGolden =
    test("SA3DiT/mediumTimestepEmbeddingMatchesPythonReference") = []
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

    auto commands = device.makeCommandBuffer();
    auto embed = Tensor::uninitializedF32({1, config.embedDim}, device);

    {
        auto pass = commands.beginCompute();
        embed = timestepEmbedding(pass, weights, 0.5f, device);
    }

    commands.commit();

    auto actual = embed.toHostF32();
    auto expected = readGoldenFloats(std::string(SA3_DIT_GOLDEN_DIR)
                                         + "/medium_timestep_embed.bin",
                                     config.embedDim);

    auto gap = maxAllcloseGap(actual, expected, 1.0e-3f, 1.0e-3f);
    check(gap <= 0.f);
};
