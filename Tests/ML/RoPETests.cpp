#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/RoPE.h>

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

std::vector<float> referenceRoPE(const std::vector<float>& x,
                                 const std::vector<float>& invFreq,
                                 int rows,
                                 int heads,
                                 int headDim)
{
    auto halfRotary = (int) invFreq.size();
    auto rotaryDim = halfRotary * 2;
    auto result = x;

    for (auto row = 0; row < rows; ++row)
        for (auto head = 0; head < heads; ++head)
        {
            auto base = (std::size_t) (row * heads + head) * headDim;

            for (auto i = 0; i < halfRotary; ++i)
            {
                auto angle = (float) row * invFreq[(std::size_t) i];
                auto cosine = std::cos(angle);
                auto sine = std::sin(angle);

                auto x1 = x[base + (std::size_t) i];
                auto x2 = x[base + (std::size_t) (halfRotary + i)];

                result[base + (std::size_t) i] = x1 * cosine - x2 * sine;
                result[base + (std::size_t) (halfRotary + i)] =
                    x2 * cosine + x1 * sine;
            }

            for (auto i = rotaryDim; i < headDim; ++i)
                result[base + (std::size_t) i] = x[base + (std::size_t) i];
        }

    return result;
}
}

auto tRoPEMatchesReferenceWithPartialRotary =
    test("RoPE/matchesReferenceWithPartialRotary") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 6;
    constexpr auto heads = 3;
    constexpr auto headDim = 64;
    constexpr auto halfRotary = 16;

    auto x = scatteredValues(rows * heads * headDim, 1);
    auto invFreqValues = std::vector<float> {};

    for (auto i = 0; i < halfRotary; ++i)
        invFreqValues.push_back(1.f / std::pow(10000.f, (float) i / halfRotary));

    auto input = Tensor::fromHostF32(x.data(), {rows, heads * headDim}, device);
    auto invFreq = Tensor::fromHostF32(invFreqValues.data(), {halfRotary}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = applyRoPE(pass, input, invFreq, heads, headDim, device);
    }

    commands.commit();

    auto values = result->toHostF32();
    auto expected = referenceRoPE(x, invFreqValues, rows, heads, headDim);

    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= 1.0e-4f);
};

auto tRoPELeavesTailUnchanged = test("RoPE/leavesTailUnchanged") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 2;
    constexpr auto heads = 1;
    constexpr auto headDim = 64;
    constexpr auto halfRotary = 16;

    auto x = scatteredValues(rows * heads * headDim, 3);
    auto invFreqValues = std::vector<float>((std::size_t) halfRotary, 0.1f);

    auto input = Tensor::fromHostF32(x.data(), {rows, heads * headDim}, device);
    auto invFreq = Tensor::fromHostF32(invFreqValues.data(), {halfRotary}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = applyRoPE(pass, input, invFreq, heads, headDim, device);
    }

    commands.commit();

    auto values = result->toHostF32();

    for (auto row = 0; row < rows; ++row)
        for (auto d = halfRotary * 2; d < headDim; ++d)
        {
            auto index = (std::size_t) row * headDim + (std::size_t) d;
            check(values[index] == x[index]);
        }
};
