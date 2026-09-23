#include <NanoTest/NanoTest.h>

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <DiT/SA3DiT.h>
#include <DiT/Weights.h>

#include "GoldenIO.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;
using namespace eacp::SA3DiT;
using namespace eacp::SA3DiT::TestSupport;

auto tGlobalConditioningMatchesGolden =
    test("SA3DiT/globalConditioningMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_DIT_CHECKPOINT_PATH});

    if (!file.has_value())
        return;

    auto weights = loadWeights(*file, DiTConfig::smallMusic(), device);

    auto commands = device.makeCommandBuffer();
    auto base = Tensor::uninitializedF32({1, embedDim * 6}, device);

    {
        auto pass = commands.beginCompute();
        base = globalConditioning(pass, weights, 0.5f, 20.f, device);
    }

    commands.commit();

    auto actual = base.toHostF32();
    auto expected = readGoldenFloats(
        std::string(SA3_DIT_GOLDEN_DIR) + "/global_cond_base.bin", embedDim * 6);

    auto gap = maxAllcloseGap(actual, expected, 2.0e-3f, 2.0e-3f);
    check(gap <= 0.f);
};
