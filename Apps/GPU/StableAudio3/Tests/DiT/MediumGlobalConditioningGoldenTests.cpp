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

auto tMediumGlobalConditioningMatchesGolden =
    test("SA3DiT/mediumGlobalConditioningMatchesPythonReference") = []
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
    auto base = Tensor::uninitializedF32({1, config.embedDim * 6}, device);

    {
        auto pass = commands.beginCompute();
        base = globalConditioning(pass, weights, 0.5f, 20.f, device);
    }

    commands.commit();

    auto actual = base.toHostF32();
    auto expected = readGoldenFloats(std::string(SA3_DIT_GOLDEN_DIR)
                                         + "/medium_global_cond_base.bin",
                                     config.embedDim * 6);

    auto gap = maxAllcloseGap(actual, expected, 3.0e-3f, 3.0e-3f);
    check(gap <= 0.f);
};
