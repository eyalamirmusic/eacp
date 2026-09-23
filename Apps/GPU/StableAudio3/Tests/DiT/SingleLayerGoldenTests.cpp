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

auto tSingleLayerMatchesGolden = test("SA3DiT/singleLayerMatchesPythonReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto file = SafetensorsFile::open(FilePath {SA3_DIT_CHECKPOINT_PATH});

    if (!file.has_value())
        return;

    auto weights = loadWeights(*file, device);

    constexpr auto seqLen = 72;
    constexpr auto contextLen = 5;

    auto dir = std::string(SA3_DIT_GOLDEN_DIR);
    auto seqInput = readGoldenFloats(dir + "/layer0_input_seq.bin", seqLen * embedDim);
    auto contextInput = readGoldenFloats(dir + "/cross_ctx_proj.bin", contextLen * embedDim);
    auto globalCondInput =
        readGoldenFloats(dir + "/global_cond_base.bin", embedDim * 6);
    auto expected = readGoldenFloats(dir + "/layer0_out.bin", seqLen * embedDim);

    auto seqTensor = Tensor::fromHostF32(seqInput.data(), {seqLen, embedDim}, device);
    auto contextTensor =
        Tensor::fromHostF32(contextInput.data(), {contextLen, embedDim}, device);
    auto globalCondTensor =
        Tensor::fromHostF32(globalCondInput.data(), {1, embedDim * 6}, device);

    auto commands = device.makeCommandBuffer();
    auto out = Tensor::uninitializedF32({seqLen, embedDim}, device);

    {
        auto pass = commands.beginCompute();
        out = transformerBlock(pass,
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
    auto gap = maxAllcloseGap(actual, expected, 5.0e-3f, 5.0e-3f);
    check(gap <= 0.f);
};
