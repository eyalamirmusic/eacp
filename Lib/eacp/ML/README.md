# ML

Tensors, a safetensors loader and the kernels a transformer is built from —
linear layers, norms, activations, RoPE, attention — written in the compute
EDSL of `eacp-gpu` (see `Lib/eacp/GPU/README.md`, "Compute"). Each kernel is a
`GPU::ComputeProgram` with its uniforms public, and each has a free function
beside it (`linear`, `applyRoPE`, `attention`, …) that allocates the result,
binds the kernel from the device's cache and records the dispatch into the pass
it is handed. Nothing commits: a whole layer, or a whole stack of them, goes
into one command buffer.

## Binding a tensor

A `Tensor` is a shape and a dtype over bytes of a GPU buffer, and those bytes
need not start at the buffer's beginning: `byteOffset()` says where they do,
and several tensors can share one buffer (see "Weights from a safetensors
file"). A kernel binds a tensor by its range, so assigning one to a buffer
uniform binds exactly its bytes wherever they lie:

```cpp
kernel.input = input;      // input.range(): {buffer, byteOffset, byteCount}
kernel.output = result;
```

There is no `buffer()` to bind instead, because binding the whole buffer is
the one mistake an offset makes easy. A `TensorView`'s `range()` is the range
of the tensor it views, and its `rowStride` and `columnOffset` count from there.

## Weights from a safetensors file

`SafetensorsFile::open` maps the file; `loadF32` is the one call per tensor:

```cpp
auto file = SafetensorsFile::open(path);
auto weight = file->loadF32("layers.0.attn.to_qkv.weight");
```

On a device that can adopt host memory (Metal), every F32 tensor is a range of
a GPU buffer over the mapping. `open` cuts the file into segments of
neighbouring tensors of up to 256 MB, and a segment becomes a buffer on the
first load from it. Nothing is copied, each buffer asks for residency in the
background (see "A buffer over memory you already have" in
`Lib/eacp/GPU/README.md`), and the buffers hold the mapping, so the tensors
outlive the `SafetensorsFile` they came from. Segments nobody loads from are
never wired: Stable Audio 3 medium's codec encoder, the last 1.7 GB of its
9.2 GB checkpoint, is never touched. And a command buffer that reads one small
tensor waits for its own segment's residency rather than the whole file's, so
the prompt encoder, which reads the conditioner's padding embedding, no longer
waits half a second behind the DiT. A tensor whose offset is
off `Device::storageBufferOffsetAlignment()` is copied into a buffer of its own,
as is every tensor on a device that cannot adopt memory, and F16 and BF16 are
converted to F32 on the host - the caller writes the same line in every case.
`loadCounts()` says how many of each there were. `readF32` is the host copy, in
F32 whatever the storage, for weights a loader transforms before upload.

## Tensor ops

`Kernels/TensorOps.h` is the arithmetic and plumbing between the layers, one
function each, so a model's own files hold only what is its own:

```cpp
auto x1 = add(pass, x, attentionOut);
auto step = scaleAndAdd(pass, x, 1.f, velocity, -t);
auto seq = concatRows(pass, {memoryTokens, x});
auto tail = sliceRows(pass, seq, memoryTokens.rows(), latentRows);
auto flat = reshape(std::move(attentionOut), {rows, heads * headDim});
```

`add`, `subtract`, `multiply` and `scaleAndAdd` are element by element;
`zeros`, `fill`, `sliceRows`, `sliceColumns`, `concatRows`, `copyRowsInto` and
`padRowsWithZeros` move rows and columns about; `reshape` hands the same buffer
back under another shape without a dispatch.

## Norms per head

`rmsNorm`, `layerNorm` and `dynamicTanh` normalise each row. The QK norm a
transformer applies before attention normalises each *head* instead — every
`headDim` values of a `rows x (heads * headDim)` query or key, with one
`headDim`-long gamma — and `rmsNormPerHead` and `dynamicTanhPerHead` are that,
with the same kernels and the input's shape back:

```cpp
auto q = rmsNormPerHead(pass, query, qNormGamma, headDim, epsilon);
```

## Column views

A fused projection puts q, k and v side by side in one `rows x 3·dim` tensor.
`qkv.columns(first, count)` is a `TensorView` of some of those columns, read
where they lie: `rmsNormPerHead`, `dynamicTanhPerHead` and the value of
`attention`, `bandedAttention` and `attendWithScores` all take one, so
splitting a projection costs no dispatch at all.

```cpp
auto qkv = linear(pass, x, qkvWeight);
auto q = rmsNormPerHead(pass, qkv.columns(0, dim), qNorm, headDim, eps);
auto k = rmsNormPerHead(pass, qkv.columns(dim, dim), kNorm, headDim, eps);
auto out = attention(pass, q, k, qkv.columns(2 * dim, dim), heads, headDim);
```

A `Tensor` converts to a view of the whole of itself, so callers holding a
tensor change nothing. The kernels only change where each value is read from;
every sum runs in the order it did over a copy, and gives the same bits.

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
