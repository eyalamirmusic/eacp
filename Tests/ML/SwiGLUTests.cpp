#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/SwiGLU.h>

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

float silu(float x)
{
    return x / (1.f + std::exp(-x));
}

std::vector<float> referenceSwiGLU(const std::vector<float>& x,
                                   const std::vector<float>& w0,
                                   const std::vector<float>& b0,
                                   const std::vector<float>& w2,
                                   const std::vector<float>& b2,
                                   int rows,
                                   int dim,
                                   int inner)
{
    auto gated = std::vector<float>((std::size_t) rows * inner);

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < inner; ++c)
        {
            auto valueA = (double) b0[(std::size_t) c];
            auto valueB = (double) b0[(std::size_t) (inner + c)];

            for (auto k = 0; k < dim; ++k)
            {
                auto xv = (double) x[(std::size_t) r * dim + k];
                valueA += xv * (double) w0[(std::size_t) c * dim + k];
                valueB += xv * (double) w0[(std::size_t) (inner + c) * dim + k];
            }

            gated[(std::size_t) r * inner + c] =
                (float) valueA * silu((float) valueB);
        }

    auto result = std::vector<float>((std::size_t) rows * dim);

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < dim; ++c)
        {
            auto value = (double) b2[(std::size_t) c];

            for (auto k = 0; k < inner; ++k)
                value += (double) gated[(std::size_t) r * inner + k]
                       * (double) w2[(std::size_t) c * inner + k];

            result[(std::size_t) r * dim + c] = (float) value;
        }

    return result;
}
}

auto tSwiGLUMatchesReference = test("SwiGLU/matchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 4;
    constexpr auto dim = 12;
    constexpr auto inner = 20;

    auto x = scatteredValues(rows * dim, 1);
    auto w0 = scatteredValues(2 * inner * dim, 2);
    auto b0 = scatteredValues(2 * inner, 3);
    auto w2 = scatteredValues(dim * inner, 5);
    auto b2 = scatteredValues(dim, 7);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto proj0Weight = Tensor::fromHostF32(w0.data(), {2 * inner, dim}, device);
    auto proj0Bias = Tensor::fromHostF32(b0.data(), {2 * inner}, device);
    auto proj2Weight = Tensor::fromHostF32(w2.data(), {dim, inner}, device);
    auto proj2Bias = Tensor::fromHostF32(b2.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();

        result = swiGLU(
            pass, input, proj0Weight, proj0Bias, proj2Weight, proj2Bias, device);
    }

    commands.commit();

    auto values = result->toHostF32();
    auto expected = referenceSwiGLU(x, w0, b0, w2, b2, rows, dim, inner);

    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= 2.0e-3f * (dim + inner));
};
