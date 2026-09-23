#include "SwiGLU.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"
#include "Linear.h"

namespace eacp::ML
{
using namespace eacp::GPU;

SwiGLUGateKernel::SwiGLUGateKernel()
{
    compile();
}

void SwiGLUGateKernel::dispatch(ComputePass& pass, int rows, int inner)
{
    innerDimension = (std::uint32_t) inner;
    pass.dispatch(*this, rows * inner);
}

void SwiGLUGateKernel::define()
{
    auto i = threadId();
    auto col = i % innerDimension;
    auto row = i / innerDimension;

    auto base = row * innerDimension * 2u;
    auto a = hidden[base + col];
    auto b = hidden[base + innerDimension + col];
    auto silu = b / (1.f + exp(-b));

    write(output, i, a * silu);
}

Tensor swiGLU(ComputePass& pass,
             const Tensor& input,
             const Tensor& proj0Weight,
             const Tensor& proj0Bias,
             const Tensor& proj2Weight,
             const Tensor& proj2Bias,
             Device& device)
{
    auto rows = input.rows();
    auto inner = proj0Weight.dim(0) / 2;

    auto hidden = linear(pass, input, proj0Weight, &proj0Bias, device);
    auto gated = Tensor::uninitializedF32({rows, inner}, device);

    auto& gateKernel = cachedKernel<SwiGLUGateKernel>(device);
    gateKernel.hidden = hidden.buffer();
    gateKernel.output = gated.buffer();
    gateKernel.dispatch(pass, rows, inner);

    return linear(pass, gated, proj2Weight, &proj2Bias, device);
}
}
