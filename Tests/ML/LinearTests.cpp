#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Linear.h>

#include <cmath>
#include <cstring>
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

// An inner dimension that is a multiple of four, so the four-wide reads are
// taken, but not of the slab, so the last slab runs past k; with rows and
// columns that leave the last output fragments hanging off the edge.
auto tLinearMatchesReferenceFourWideRagged =
    test("Linear/matchesReferenceFourWideRagged") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 339;
    constexpr auto inner = 36;
    constexpr auto columns = 70;

    auto x = scatteredValues(rows * inner, 5);
    auto w = scatteredValues(columns * inner, 9);

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

namespace
{
std::vector<float> roundingValues(int count, float salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back(std::sin((float) i * 0.377f + salt) * 1.7f);

    return values;
}

std::vector<float> runLinearF32(Device& device,
                                int tileRows,
                                const Tensor& input,
                                const Tensor& weight)
{
    auto kernel = LinearF32 {LinearLoads::FourWide, tileRows};
    kernel.prepare(device);

    auto result = Tensor::uninitializedF32({input.rows(), weight.dim(0)}, device);
    kernel.activations = input.buffer();
    kernel.weight = weight.buffer();
    kernel.output = result.buffer();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        kernel.dispatch(pass, input.rows(), weight.dim(0), input.cols());
    }

    commands.commit();
    return result.toHostF32();
}
} // namespace

// Tiles 32 and 64 rows tall split the output differently among SIMD groups, but
// every element is the same sequence of products, so the two agree to the bit.
// The values round, so a different order of sums would show.
auto tLinearTilingsGiveTheSameBits = test("Linear/tilingsGiveTheSameBits") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 99;
    constexpr auto inner = 76;
    constexpr auto columns = 136;

    auto x = roundingValues(rows * inner, 0.3f);
    auto w = roundingValues(columns * inner, 1.1f);

    auto input = Tensor::fromHostF32(x.data(), {rows, inner}, device);
    auto weight = Tensor::fromHostF32(w.data(), {columns, inner}, device);

    auto tall = runLinearF32(device, 64, input, weight);
    auto short32 = runLinearF32(device, 32, input, weight);

    check(tall.size() == short32.size());
    check(std::memcmp(tall.data(), short32.data(), tall.size() * sizeof(float))
          == 0);
    checkMatches(
        tall, referenceLinear(x, w, nullptr, rows, inner, columns), 1.0e-3f);
};

auto tLinearTileRowsPadTheBatchLeast = test("Linear/tileRowsPadTheBatchLeast") = []
{
    check(linearTileRowsFor(387, 12288) == 32);
    check(linearTileRowsFor(384, 12288) == 64);
    check(linearTileRowsFor(5491, 1536) == 64);
    check(linearTileRowsFor(1, 12288) == 32);
    check(linearTileRowsFor(1024, 3072) == 64);
};

auto tLinearTileRowsFillTheGpu = test("Linear/tileRowsFillTheGpu") = []
{
    check(linearTileRowsFor(384, 1536) == 32);
    check(linearTileRowsFor(256, 768) == 32);
    check(linearTileRowsFor(384, 6144) == 64);
};
