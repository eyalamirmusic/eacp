#include "GemmaAttention.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/Attention.h>

namespace eacp::SA3TextEncoder
{
using namespace eacp::GPU;
using namespace eacp::ML;

GemmaAttentionScoresKernel::GemmaAttentionScoresKernel()
{
    compile();
}

void GemmaAttentionScoresKernel::dispatch(ComputePass& pass,
                                          int rows,
                                          int heads,
                                          int cols)
{
    headCount = (std::uint32_t) heads;
    columnCount = (std::uint32_t) cols;
    pass.dispatch(*this, rows * heads * cols);
}

void GemmaAttentionScoresKernel::define()
{
    auto i = threadId();
    auto col = i % columnCount;
    auto rowHead = i / columnCount;
    auto row = rowHead / headCount;

    auto queryBase = rowHead * headDimension;
    auto head = rowHead % headCount;
    auto keyBase = (col * headCount + head) * headDimension;

    auto dot = var(0.f);
    auto d = var(0u);

    loop(d.get() < headDimension,
         [&]
         {
             dot = dot.get() + query[queryBase + d.get()] * key[keyBase + d.get()];
             d = d.get() + 1u;
         });

    auto scaled = dot.get() * scale;
    auto capped = softcap * tanh(scaled / softcap);
    auto score = capped + additiveMask[row * columnCount + col];
    write(scores, i, score);
}

ML::Tensor gemmaSelfAttention(ComputePass& pass,
                              const Tensor& query,
                              const Tensor& key,
                              const Tensor& value,
                              const Tensor& additiveMask,
                              int heads,
                              int headDim,
                              float scale,
                              float softcap,
                              Device& device)
{
    auto rows = query.rows();
    auto cols = key.rows();

    auto scores = Tensor::uninitializedF32({rows, heads, cols}, device);

    auto& scoresKernel = GPU::sharedKernel<GemmaAttentionScoresKernel>(device);
    scoresKernel.query = query;
    scoresKernel.key = key;
    scoresKernel.additiveMask = additiveMask;
    scoresKernel.scores = scores;
    scoresKernel.headDimension = (std::uint32_t) headDim;
    scoresKernel.scale = scale;
    scoresKernel.softcap = softcap;
    scoresKernel.dispatch(pass, rows, heads, cols);

    return attendWithScores(pass, scores, value, heads, headDim, device);
}
} // namespace eacp::SA3TextEncoder
