#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>

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

std::vector<float> referenceLinear(const std::vector<float>& x,
                                   const std::vector<float>& w,
                                   const std::vector<float>* bias,
                                   int rows,
                                   int inner,
                                   int columns)
{
    auto result = std::vector<float>((std::size_t) rows * columns);

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < columns; ++c)
        {
            auto total = (double) (bias != nullptr ? (*bias)[(std::size_t) c] : 0.f);

            for (auto k = 0; k < inner; ++k)
                total += (double) x[(std::size_t) r * inner + k]
                        * (double) w[(std::size_t) c * inner + k];

            result[(std::size_t) r * columns + c] = (float) total;
        }

    return result;
}

void checkMatches(const std::vector<float>& values,
                  const std::vector<float>& expected,
                  float tolerance)
{
    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= tolerance);
}

Tensor runLinear(Device& device,
                 const Tensor& input,
                 const Tensor& weight,
                 const Tensor* bias)
{
    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = linear(pass, input, weight, bias, device);
    }

    commands.commit();
    return std::move(*result);
}
}

auto tLinearMatchesReferenceAtSmallShape = test("Linear/matchesReferenceSmall") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;
    constexpr auto inner = 7;
    constexpr auto columns = 6;

    auto x = scatteredValues(rows * inner, 1);
    auto w = scatteredValues(columns * inner, 2);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostF32(w.data(), {columns, inner}, device);
    auto result = runLinear(device, input, weight, nullptr);

    checkMatches(result.toHostF32(),
                referenceLinear(x, w, nullptr, rows, inner, columns),
                2.0e-4f * inner);
};

auto tLinearMatchesReferenceAtRaggedShape = test("Linear/matchesReferenceRagged") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 100;
    constexpr auto inner = 45;
    constexpr auto columns = 76;

    auto x = scatteredValues(rows * inner, 7);
    auto w = scatteredValues(columns * inner, 11);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostF32(w.data(), {columns, inner}, device);
    auto result = runLinear(device, input, weight, nullptr);

    checkMatches(result.toHostF32(),
                referenceLinear(x, w, nullptr, rows, inner, columns),
                2.0e-4f * inner);
};

auto tLinearMatchesReferenceWithBias = test("Linear/matchesReferenceWithBias") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 9;
    constexpr auto inner = 13;
    constexpr auto columns = 17;

    auto x = scatteredValues(rows * inner, 3);
    auto w = scatteredValues(columns * inner, 5);
    auto b = scatteredValues(columns, 9);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostF32(w.data(), {columns, inner}, device);
    auto bias = Tensor::fromHostF32(b.data(), {columns}, device);
    auto result = runLinear(device, input, weight, &bias);

    checkMatches(result.toHostF32(),
                referenceLinear(x, w, &b, rows, inner, columns),
                2.0e-4f * inner);
};

auto tLinearMatchesReferenceAtModelShape = test("Linear/matchesReferenceAtModelShape") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 8;
    constexpr auto inner = 1024;
    constexpr auto columns = 1024;

    auto x = scatteredValues(rows * inner, 13);
    auto w = scatteredValues(columns * inner, 17);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostF32(w.data(), {columns, inner}, device);
    auto result = runLinear(device, input, weight, nullptr);

    checkMatches(result.toHostF32(),
                referenceLinear(x, w, nullptr, rows, inner, columns),
                2.0e-4f * inner);
};

auto tLinearPackedHalfWeightIsCloseToReference =
    test("Linear/packedHalfWeightIsCloseToReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 6;
    constexpr auto inner = 33;
    constexpr auto columns = 20;

    auto x = scatteredValues(rows * inner, 19);
    auto w = scatteredValues(columns * inner, 23);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostPackedF16(w.data(), {columns, inner}, device);
    auto result = runLinear(device, input, weight, nullptr);

    checkMatches(result.toHostF32(),
                referenceLinear(x, w, nullptr, rows, inner, columns),
                5.0e-2f + 5.0e-3f * inner);
};
