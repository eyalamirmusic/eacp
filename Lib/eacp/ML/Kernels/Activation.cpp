#include "Activation.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

namespace
{
Float sigmoidOf(const Float& x)
{
    return 1.f / (1.f + exp(-x));
}

constexpr auto geluTanhCoefficient = 0.7978845608028654f;
constexpr auto geluCubicCoefficient = 0.044715f;
constexpr auto inverseSqrtTwo = 0.7071067811865476f;
}

ActivationKernel::ActivationKernel(ActivationKind kindToUse)
    : kind(kindToUse)
{
    compile();
}

void ActivationKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

void ActivationKernel::define()
{
    auto i = threadId();
    auto x = input[i];

    switch (kind)
    {
        case ActivationKind::SiLU:
            write(output, i, x * sigmoidOf(x));
            break;

        case ActivationKind::GeluTanh:
        {
            auto inner =
                geluTanhCoefficient * (x + geluCubicCoefficient * x * x * x);
            write(output, i, 0.5f * x * (1.f + tanh(inner)));
            break;
        }

        case ActivationKind::GeluExact:
            write(output, i, 0.5f * x * (1.f + erf(x * inverseSqrtTwo)));
            break;

        case ActivationKind::Sigmoid:
            write(output, i, sigmoidOf(x));
            break;
    }
}

Tensor applyActivation(ComputePass& pass,
                       const Tensor& input,
                       ActivationKind kind,
                       Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = cachedKernel<ActivationKernel>(device, kind);
    kernel.input = input.buffer();
    kernel.output = result.buffer();
    kernel.dispatch(pass, input.count());

    return result;
}
}
