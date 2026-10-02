#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CommandBuffer/CommandBuffer.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Norm.h>

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

std::vector<float> referenceRMSNorm(const std::vector<float>& x,
                                    const std::vector<float>& gamma,
                                    int rows,
                                    int dim,
                                    float eps)
{
    auto result = std::vector<float>((std::size_t) rows * dim);

    for (auto r = 0; r < rows; ++r)
    {
        auto sumSquares = 0.0;

        for (auto c = 0; c < dim; ++c)
        {
            auto value = (double) x[(std::size_t) r * dim + c];
            sumSquares += value * value;
        }

        auto scale = 1.0 / std::sqrt(sumSquares / dim + (double) eps);

        for (auto c = 0; c < dim; ++c)
            result[(std::size_t) r * dim + c] =
                (float) ((double) x[(std::size_t) r * dim + c] * scale
                        * (double) gamma[(std::size_t) c]);
    }

    return result;
}

std::vector<float> referenceLayerNorm(const std::vector<float>& x,
                                      const std::vector<float>& gamma,
                                      const std::vector<float>* beta,
                                      int rows,
                                      int dim,
                                      float eps)
{
    auto result = std::vector<float>((std::size_t) rows * dim);

    for (auto r = 0; r < rows; ++r)
    {
        auto mean = 0.0;

        for (auto c = 0; c < dim; ++c)
            mean += (double) x[(std::size_t) r * dim + c];

        mean /= dim;

        auto variance = 0.0;

        for (auto c = 0; c < dim; ++c)
        {
            auto centred = (double) x[(std::size_t) r * dim + c] - mean;
            variance += centred * centred;
        }

        variance /= dim;

        auto scale = 1.0 / std::sqrt(variance + (double) eps);

        for (auto c = 0; c < dim; ++c)
        {
            auto centred = (double) x[(std::size_t) r * dim + c] - mean;
            auto value = centred * scale * (double) gamma[(std::size_t) c];

            if (beta != nullptr)
                value += (double) (*beta)[(std::size_t) c];

            result[(std::size_t) r * dim + c] = (float) value;
        }
    }

    return result;
}

std::vector<float> referenceDynamicTanh(const std::vector<float>& x,
                                        const std::vector<float>& gamma,
                                        const std::vector<float>& beta,
                                        int rows,
                                        int dim,
                                        float alpha)
{
    auto result = std::vector<float>((std::size_t) rows * dim);

    for (auto r = 0; r < rows; ++r)
        for (auto c = 0; c < dim; ++c)
        {
            auto value = x[(std::size_t) r * dim + c];

            result[(std::size_t) r * dim + c] =
                std::tanh(alpha * value) * gamma[(std::size_t) c]
                + beta[(std::size_t) c];
        }

    return result;
}
}

auto tRMSNormMatchesReferenceAtHeadDim = test("Norm/rmsNormMatchesReferenceHeadDim") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;
    constexpr auto dim = 64;
    constexpr auto eps = 1.0e-6f;

    auto x = scatteredValues(rows * dim, 1);
    auto gamma = scatteredValues(dim, 2);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto gammaTensor = Tensor::fromHostF32(gamma.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = rmsNorm(pass, input, gammaTensor, eps, device);
    }

    commands.commit();

    checkMatches(result->toHostF32(),
                referenceRMSNorm(x, gamma, rows, dim, eps),
                1.0e-4f);
};

auto tRMSNormMatchesReferenceAtEmbedDim =
    test("Norm/rmsNormMatchesReferenceEmbedDim") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 3;
    constexpr auto dim = 1024;
    constexpr auto eps = 1.0e-6f;

    auto x = scatteredValues(rows * dim, 3);
    auto gamma = scatteredValues(dim, 5);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto gammaTensor = Tensor::fromHostF32(gamma.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = rmsNorm(pass, input, gammaTensor, eps, device);
    }

    commands.commit();

    checkMatches(result->toHostF32(),
                referenceRMSNorm(x, gamma, rows, dim, eps),
                1.0e-4f);
};

auto tLayerNormMatchesReferenceWithBias = test("Norm/layerNormMatchesReferenceWithBias") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 4;
    constexpr auto dim = 96;
    constexpr auto eps = 1.0e-5f;

    auto x = scatteredValues(rows * dim, 7);
    auto gamma = scatteredValues(dim, 9);
    auto beta = scatteredValues(dim, 11);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto gammaTensor = Tensor::fromHostF32(gamma.data(), {dim}, device);
    auto betaTensor = Tensor::fromHostF32(beta.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = layerNorm(pass, input, gammaTensor, &betaTensor, eps, device);
    }

    commands.commit();

    checkMatches(result->toHostF32(),
                referenceLayerNorm(x, gamma, &beta, rows, dim, eps),
                1.0e-4f);
};

auto tLayerNormMatchesReferenceWithoutBias =
    test("Norm/layerNormMatchesReferenceWithoutBias") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 4;
    constexpr auto dim = 96;
    constexpr auto eps = 1.0e-5f;

    auto x = scatteredValues(rows * dim, 13);
    auto gamma = scatteredValues(dim, 15);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto gammaTensor = Tensor::fromHostF32(gamma.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = layerNorm(pass, input, gammaTensor, nullptr, eps, device);
    }

    commands.commit();

    checkMatches(result->toHostF32(),
                referenceLayerNorm(x, gamma, nullptr, rows, dim, eps),
                1.0e-4f);
};

auto tDynamicTanhMatchesReference = test("Norm/dynamicTanhMatchesReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto rows = 5;
    constexpr auto dim = 48;
    constexpr auto alpha = 0.6f;

    auto x = scatteredValues(rows * dim, 17);
    auto gamma = scatteredValues(dim, 19);
    auto beta = scatteredValues(dim, 21);

    auto input = Tensor::fromHostF32(x.data(), {rows, dim}, device);
    auto gammaTensor = Tensor::fromHostF32(gamma.data(), {dim}, device);
    auto betaTensor = Tensor::fromHostF32(beta.data(), {dim}, device);

    auto commands = device.makeCommandBuffer();
    auto result = std::optional<Tensor> {};

    {
        auto pass = commands.beginCompute();
        result = dynamicTanh(pass, input, gammaTensor, betaTensor, alpha, device);
    }

    commands.commit();

    checkMatches(result->toHostF32(),
                referenceDynamicTanh(x, gamma, beta, rows, dim, alpha),
                1.0e-5f);
};
