#include "Ops.h"

#include "../GPU/Frame/ComputePass.h"

namespace eacp::SA3Sampler
{
using namespace eacp::GPU;
using namespace eacp::ML;

ScaleAndAddKernel::ScaleAndAddKernel()
{
    compile();
}

void ScaleAndAddKernel::dispatch(ComputePass& pass,
                                 int count,
                                 float scaleAValue,
                                 float scaleBValue)
{
    scaleA = scaleAValue;
    scaleB = scaleBValue;
    pass.dispatch(*this, count);
}

void ScaleAndAddKernel::define()
{
    auto i = threadId();
    write(output, i, a[i] * scaleA + b[i] * scaleB);
}

Tensor scaleAndAdd(ComputePass& pass,
                   const Tensor& a,
                   float scaleA,
                   const Tensor& b,
                   float scaleB,
                   Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto kernel = ScaleAndAddKernel {};
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, a.count(), scaleA, scaleB);

    return result;
}
}
