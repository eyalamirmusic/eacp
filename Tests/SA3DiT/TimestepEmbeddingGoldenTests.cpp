#include <NanoTest/NanoTest.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/SA3DiT/SA3DiT.h>
#include <eacp/SA3DiT/Weights.h>

#include "GoldenIO.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;
using namespace eacp::SA3DiT::TestSupport;

auto tTimestepEmbeddingMatchesGolden =
    test("SA3DiT/timestepEmbeddingMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_DIT_CHECKPOINT_PATH});

    if (!file.has_value())
        return;

    auto weights = loadWeights(*file, device);

    auto commands = device.makeCommandBuffer();
    auto embed = Tensor::uninitializedF32({1, embedDim}, device);

    {
        auto pass = commands.beginCompute();
        embed = timestepEmbedding(pass, weights, 0.5f, device);
    }

    commands.commit();

    auto actual = embed.toHostF32();
    auto expected = readGoldenFloats(
        std::string(SA3_DIT_GOLDEN_DIR) + "/timestep_embed.bin", embedDim);

    auto gap = maxAllcloseGap(actual, expected, 1.0e-3f, 1.0e-3f);
    check(gap <= 0.f);
};
