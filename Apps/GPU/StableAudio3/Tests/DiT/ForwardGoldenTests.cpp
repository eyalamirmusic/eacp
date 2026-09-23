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

auto tForwardMatchesGolden = test("SA3DiT/fullForwardMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_DIT_CHECKPOINT_PATH});

    if (!file.has_value())
        return;

    auto weights = loadWeights(*file, device);

    constexpr auto latentLength = 8;
    constexpr auto contextLen = 5;

    auto dir = std::string(SA3_DIT_GOLDEN_DIR);
    auto latentInput = readGoldenFloats(dir + "/latent.bin", latentLength * ioChannels);
    auto contextInput = readGoldenFloats(dir + "/cross_ctx_raw.bin", contextLen * condTokenDim);
    auto expected = readGoldenFloats(dir + "/full_out.bin", latentLength * ioChannels);

    auto latentTensor =
        Tensor::fromHostF32(latentInput.data(), {latentLength, ioChannels}, device);
    auto contextTensor =
        Tensor::fromHostF32(contextInput.data(), {contextLen, condTokenDim}, device);

    auto commands = device.makeCommandBuffer();
    auto out = Tensor::uninitializedF32({latentLength, ioChannels}, device);

    {
        auto pass = commands.beginCompute();
        out = forward(pass, weights, latentTensor, 0.5f, 20.f, contextTensor, device);
    }

    commands.commit();

    auto actual = out.toHostF32();
    auto gap = maxAllcloseGap(actual, expected, 1.0e-2f, 1.0e-2f);
    check(gap <= 0.f);
};
