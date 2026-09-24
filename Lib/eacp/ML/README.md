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
