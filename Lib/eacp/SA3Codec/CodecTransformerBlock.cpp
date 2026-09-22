#include "CodecTransformerBlock.h"

#include "GpuOps.h"

#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Attention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/RoPE.h>
#include <eacp/ML/Kernels/SwiGLU.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
Tensor dynamicTanhPerHead(ComputePass& pass,
                          const Tensor& input,
                          const DynamicTanhWeights& norm,
                          int rowCount,
                          int heads,
                          int headDim,
                          Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto kernel = DynamicTanhKernel {};
    kernel.input = input.buffer();
    kernel.gamma = norm.gamma.buffer();
    kernel.beta = norm.beta.buffer();
    kernel.output = result.buffer();
    kernel.alpha = norm.alpha;
    kernel.prepare(device);
    kernel.dispatch(pass, rowCount * heads, headDim);

    return result;
}
}

Tensor applyCodecTransformerBlock(ComputePass& pass,
                                  const Tensor& input,
                                  const CodecBlockWeights& weights,
                                  Device& device)
{
    auto dim = weights.heads * weights.headDim;
    auto rows = input.rows();

    auto normed = dynamicTanh(
        pass, input, weights.preNorm.gamma, weights.preNorm.beta, weights.preNorm.alpha, device);
    auto qkv = linear(pass, normed, weights.qkvWeight, nullptr, device);

    auto qTensor = sliceColumnsGpu(pass, qkv, 0 * dim, dim, device);
    auto kTensor = sliceColumnsGpu(pass, qkv, 1 * dim, dim, device);
    auto vTensor = sliceColumnsGpu(pass, qkv, 2 * dim, dim, device);
    auto qDiffTensor = sliceColumnsGpu(pass, qkv, 3 * dim, dim, device);
    auto kDiffTensor = sliceColumnsGpu(pass, qkv, 4 * dim, dim, device);

    auto qNormed =
        dynamicTanhPerHead(pass, qTensor, weights.qNorm, rows, weights.heads, weights.headDim, device);
    auto kNormed =
        dynamicTanhPerHead(pass, kTensor, weights.kNorm, rows, weights.heads, weights.headDim, device);
    auto qDiffNormed = dynamicTanhPerHead(
        pass, qDiffTensor, weights.qNorm, rows, weights.heads, weights.headDim, device);
    auto kDiffNormed = dynamicTanhPerHead(
        pass, kDiffTensor, weights.kNorm, rows, weights.heads, weights.headDim, device);

    auto qRoped = applyRoPE(pass, qNormed, weights.invFreq, weights.heads, weights.headDim, device);
    auto kRoped = applyRoPE(pass, kNormed, weights.invFreq, weights.heads, weights.headDim, device);
    auto qDiffRoped =
        applyRoPE(pass, qDiffNormed, weights.invFreq, weights.heads, weights.headDim, device);
    auto kDiffRoped =
        applyRoPE(pass, kDiffNormed, weights.invFreq, weights.heads, weights.headDim, device);

    auto zeroMask = buildZeroMask(rows, rows, device);

    auto primaryOut = attention(pass,
                               qRoped,
                               kRoped,
                               vTensor,
                               weights.heads,
                               weights.headDim,
                               &zeroMask,
                               nullptr,
                               nullptr,
                               1e-6f,
                               device);

    auto diffOut = attention(pass,
                             qDiffRoped,
                             kDiffRoped,
                             vTensor,
                             weights.heads,
                             weights.headDim,
                             &zeroMask,
                             nullptr,
                             nullptr,
                             1e-6f,
                             device);

    auto differential = subtractTensorsGpu(pass, primaryOut, diffOut, device);
    auto differentialFlat = reshapeFlat(std::move(differential), {rows, dim});

    auto attnProjected = linear(pass, differentialFlat, weights.toOutWeight, nullptr, device);
    auto afterAttention = addTensorsGpu(pass, input, attnProjected, device);

    auto ffNormed = dynamicTanh(pass,
                               afterAttention,
                               weights.ffNorm.gamma,
                               weights.ffNorm.beta,
                               weights.ffNorm.alpha,
                               device);
    auto ffOutput = swiGLU(
        pass, ffNormed, weights.ff0Weight, weights.ff0Bias, weights.ff2Weight, weights.ff2Bias, device);

    return addTensorsGpu(pass, afterAttention, ffOutput, device);
}
}
