#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Activation.h>

#include <cmath>
#include <optional>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back((float) (((i * 37 + salt * 11) % 23) - 11) * 0.125f);

    return values;
}

float sigmoidOf(float x)
{
    return 1.f / (1.f + std::exp(-x));
}

float referenceForKind(ActivationKind kind, float x)
{
    switch (kind)
    {
        case ActivationKind::SiLU:
            return x * sigmoidOf(x);

        case ActivationKind::GeluTanh:
        {
            constexpr auto coefficient = 0.7978845608028654f;
            auto inner = coefficient * (x + 0.044715f * x * x * x);
            return 0.5f * x * (1.f + std::tanh(inner));
        }

        case ActivationKind::GeluExact:
            return 0.5f * x * (1.f + std::erf(x * 0.7071067811865476f));

        case ActivationKind::Sigmoid:
            return sigmoidOf(x);
    }

    return 0.f;
}

void checkActivation(ActivationKind kind, float tolerance)
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 40;

    auto x = scatteredValues(count, (int) kind + 1);
    auto input = Tensor::fromHostF32(x.data(), {count}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = applyActivation(pass, input, kind, device);
    }

    commands.commit();
    auto values = result->toHostF32();

    for (auto i = 0; i < count; ++i)
    {
        auto expected = referenceForKind(kind, x[(std::size_t) i]);
        check(std::abs(values[(std::size_t) i] - expected) <= tolerance);
    }
}
}

auto tSiLUMatchesReference = test("Activation/siluMatchesReference") = []
{
    checkActivation(ActivationKind::SiLU, 1.0e-5f);
};

auto tGeluTanhMatchesReference = test("Activation/geluTanhMatchesReference") = []
{
    checkActivation(ActivationKind::GeluTanh, 1.0e-5f);
};

auto tGeluExactMatchesReference = test("Activation/geluExactMatchesReference") = []
{
    checkActivation(ActivationKind::GeluExact, 1.0e-5f);
};

auto tSigmoidMatchesReference = test("Activation/sigmoidMatchesReference") = []
{
    checkActivation(ActivationKind::Sigmoid, 1.0e-5f);
};
