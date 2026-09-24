# ML

Tensors, a safetensors loader and the kernels a transformer is built from —
linear layers, norms, activations, RoPE, attention — written in the compute
EDSL of `eacp-gpu` (see `Lib/eacp/GPU/README.md`, "Compute"). Each kernel is a
`GPU::ComputeProgram` with its uniforms public, and each has a free function
beside it (`linear`, `applyRoPE`, `attention`, …) that allocates the result,
binds the kernel from the device's cache and records the dispatch into the pass
it is handed. Nothing commits: a whole layer, or a whole stack of them, goes
into one command buffer.

## RoPE over segments

`applyRoPE` rotates each row by its position. Several independent sequences
stacked into one tensor — every chunk of a chunked decoder, a batch of prompts
of one length — want each row rotated by its position *within its sequence*,
and the overload that takes a segment length does exactly that:

```cpp
auto q = applyRoPE(pass, query, invFreq, heads, headDim, chunkRows);
```

Row `r` takes position `r % chunkRows`, so every chunk goes through in one
dispatch and comes out bit for bit as it would have alone. A segment length of
0 is the whole tensor, which is the overload without one. It pairs with
`bandedAttention`'s `AttentionBand::segmentRows`: the same number keeps the
chunks from seeing each other.

## Attention, and each probability computed once

`attention` is three kernels: the scores, the row stats, the weighted sum. The
row stats find each (row, head)'s peak, then turn its scores into
probabilities *in place* — `exp(score - peak)`, written back over the score it
came from — and sum them. The weighted sum reads those probabilities back. Each
`exp` is evaluated once, where it used to be evaluated again by every one of
the `headDim` threads that weigh a value by it: 65 per score before, 1 now.

It gives the same bits as recomputing: the one `exp` has the same input the
weighted sum's used to, and the weighted sum still adds columns in ascending
order. `bandedAttention` does the same over its window.

The softmax and weighted sum are also a function of their own, for a model
whose scores are not `attention`'s — a soft cap, a bias, another scale:

```cpp
auto scores = Tensor::uninitializedF32({rows, heads, cols});
// ... a score kernel of the model's own writes them ...
auto output = attendWithScores(pass, scores, value, heads, headDim);
```

`scores` comes back holding the unnormalised probabilities; the output is
`rows x heads x headDim`.

The row stats write the probability and then add it to the sum, and they are
written the way the C++ would be — `auto p = exp(scores[i] - peak);`, stored,
then added. That `p` is one value, evaluated once before the store, is the
EDSL's rule rather than this kernel's care: see "A handle is a value" in
`Lib/eacp/GPU/README.md`.
