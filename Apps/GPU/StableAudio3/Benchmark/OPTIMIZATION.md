# Stable Audio 3 inference: where the time and memory go

Baseline: `--model medium --seconds 30`, 8 steps, 323 latent frames, M5 Max,
`build/` with an empty `CMAKE_BUILD_TYPE` (-O0).

| Phase | Time | Notes |
|---|---|---|
| Load DiT | 0.76 s | 5.8 GB F32 copied out of the mmap |
| Load codec | 0.49 s | encoder *and* decoder, 3.4 GB |
| Load T5Gemma | 5.46 s | almost all of it is the tokenizer JSON parse at -O0 |
| Encode prompt | 0.31 s | |
| Sampling | 9.36 s | 94–100% GPU mid-run, 97%/0% alternating in the first 2.5 s |
| Decode | 22.0 s | 35% GPU average, ~1.7 s cycle per codec layer |
| Peak RSS | 19.3 GB | GPU in use 11.9 GB sampling, 14.6 GB decode, 25 GB alloc peak |

Nothing below has been run on the GPU; the GPU was busy while this was written.
Two things were measured on the CPU only, with no Metal involved, comparing this
worktree's Release build to the main checkout's -O0 `build/`:

| CPU-only measurement | -O0 | Release |
|---|---|---|
| `BpeTokenizer::load` of the 34 MB `tokenizer.json` (`SA3TextEncoderTokenizerGoldenTests`) | 4.52 s | 0.33 s |
| Constructing one `LinearF32` (graph build and MSL emission, no Metal) | 2978 µs | 221 µs |
| Constructing one `AttentionScoresKernel` / `RMSNormKernel` | ~95 µs | ~9 µs |

Every other figure here is an estimate from the code and the shapes. Every
estimate needs checking against a Release run with Metal System Trace
(`xctrace record --template 'Metal System Trace' --launch -- build/Apps/GPU/StableAudio3/StableAudio3 --model medium --seconds 30`).

Numerics: **bit-exact** means the same bits come out, so the golden tests are
untouched. **Tolerance** means the result moves by rounding only and has to pass
the existing goldens (DiT: `maxAllcloseGap` at 3e-2 abs/rel,
`Tests/DiT/MediumForwardGoldenTests.cpp:67`; SAME-L blocks: 5e-3 / 2e-2,
`Tests/Codec/SameLGoldenTests.cpp:112,156`; T5: 0.5 / 1e-5,
`Tests/TextEncoder/FullEncoderTests.cpp:110,150`).

---

## Ranked by win / effort

### 1. Build Release — zero effort, several seconds

**Evidence.** The -O0 build is the one being measured. The CPU work sits on the
critical path in two places:

- `BpeTokenizer::load` (`TextEncoder/Tokenizer/BpeTokenizer.cpp:75-82`) builds a
  `Json` DOM (`std::map` per object, `Lib/eacp/ML/Loader/Json.h:14-15`) for a
  34 MB file with 256k vocab entries. 4.52 s at -O0, 0.33 s in Release. Most of
  the 5.46 s T5 load is this.
- Every kernel call site constructs a fresh `ComputeProgram`, whose constructor
  runs `compile()` — graph build plus MSL text emission
  (`GPU/Codegen/ComputeProgram.h:330-336`). A medium DiT step has ~170
  `linear()` calls (7 per layer × 24, plus the top-level ones) and ~1,300 other
  dispatches. At -O0 that is ~0.5 s of `LinearF32` emission plus ~0.13 s for the
  rest, **per step**. `CommandBuffer::commit()` blocks
  (`GPU/CommandBuffer/CommandBuffer-Apple.mm:131-134`), so the GPU sits idle
  while all of that runs. In Release it drops to ~50 ms per step.

**Fix.** `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DEACP_UNITY_BUILD=OFF`.
Better: default `CMAKE_BUILD_TYPE` to Release when it is empty, or at least for
the SA3 targets.

**Expected win.** T5 load −4 to −5 s. Sampling −2 to −5 s: somewhere between
the 0.66 s/step CPU estimate and the high GPU utilisation you measured, which
doesn't fit that estimate cleanly, so re-measure. Weight loading is also
somewhat faster.

**Risk.** GPU code is unchanged: the MSL emitted is the same text. The host float
code (`computeWeightNormFlat`, `loadGammaPlusOne`, bf16→f32) uses no fast-math,
so it should be **bit-exact**. Confirm with the goldens.

---

### 2. SAME-L decode: banded attention instead of dense 5491×5491 — the decode fix

**Evidence.** SAME-L uses `CodecAttentionMode::SlidingWindow` with
`slidingWindowRadiusChunks = 1` (`Codec/CodecConfig.h:55-57`). The radius is
`1 × (stride + 1) = 17` rows (`Codec/TransformerResamplingBlock.cpp:172`). The
decoder folds 323 latent frames into 323 × 17 = **5,491 rows**. Then:

- `buildSlidingWindowMaskGpu` makes a dense 5491×5491 float mask (120 MB) whose
  in-band part is 35 columns per row (`Codec/GpuOps.cpp:514-531`).
- `attention()` allocates `scores` of `rows × heads × cols` =
  5491 × 24 × 5491 × 4 B = **2.9 GB per call** (`Lib/eacp/ML/Kernels/Attention.cpp:240`).
  There are two calls per layer (differential attention,
  `Codec/CodecTransformerBlock.cpp:142-164`) and 12 layers.
- The three kernels touch every column: `AttentionScoresKernel` does a 64-wide
  dot product for each of 723 M scores, `AttentionRowStatsKernel` makes two
  passes, and `AttentionWeightedSumKernel` recomputes `exp(score − peak)` for
  every (row, head, d) — 46 G `exp`s per attention call
  (`Attention.cpp:143-170`).

Only 35/5491 = **0.64%** of that is inside the band. Per layer that is about
370 GFLOP-equivalent plus 92 G `exp`s of dense attention, against about
390 GFLOP of linears. On top of the compute, each layer's command buffer
allocates ~6 GB of fresh `scores` (see item 5 for why fresh allocation costs).
Each layer is its own blocking command buffer
(`TransformerResamplingBlock.cpp:80-90`), which matches the ~1.7 s cycle you
saw, one per layer across 12 layers ≈ 20 s.

**Fix.** Add a banded variant of the three attention kernels that loops only
over `[row − left, row + right]`. The mask becomes two uniforms, not a buffer,
and `scores` becomes `rows × heads × 35`. Also drop the per-layer command
buffers: encode all 12 layers into one.

**Bit-exactness is achievable**, and it's worth insisting on:
- Out-of-band scores are `dot·scale − 1e9`. Their `exp(· − peak)` is exactly
  `0.0f`, so they add nothing to any sum, and `rowMax` is unchanged because a
  max ignores order.
- `AttentionRowStatsKernel` sums per lane over `col ≡ lane (mod 256)`, then
  `groupSum`. With 35 < 256, each lane holds at most one in-band column. If
  the banded kernel assigns column `c` to lane `c % 256`, like today, every
  lane's partial and the `groupSum` tree are bit-identical.
- `AttentionWeightedSumKernel` accumulates in ascending column order. Skipping
  exact-zero terms leaves the accumulator bit-identical.
- In-band scores are `dot·scale + 0.0f`, the same as `dot·scale`.

**Expected win.** Decode goes from 22 s to roughly the linears alone:
~4.7 TFLOP → ~3–6 s at the current `LinearF32` efficiency, less after item 10.
Decode peak GPU memory drops by ~6 GB. The 120 MB mask goes away.

**Risk.** **Bit-exact** if the lane mapping above is kept. The goldens use
8 latent frames = 136 rows > 35, so the band edge is exercised.
`SameLGoldenTests` and `SameLDecodeVariesWithLatentTests` cover it. Medium
effort: one new kernel trio plus a call-site change in `runSlidingWindowStack`.

---

### 3. RSS: weights are held twice, plus weights that are never used

**Evidence.**
- `SafetensorsFile::loadF32` → `Tensor::fromHostF32` → `newBufferWithBytes`
  copies each tensor out of the mmap into a new shared `MTLBuffer`
  (`Lib/eacp/ML/Loader/SafetensorsFile.cpp:135-142`,
  `Lib/eacp/ML/Tensor/Tensor.cpp:24-33`,
  `Lib/eacp/GPU/Buffer/Buffer-Apple.mm:49-52`). The copy reads every page of the
  mapping, so the pages become resident. `main` keeps `file` alive until return
  (`Main.cpp:121`), so all 9.2 GB of mapped pages stay in RSS next to the GPU
  copies.
  9.2 GB mapped + ~9.5 GB of shared buffers (shared storage counts in RSS) ≈ the
  19.3 GB measured.
- `SameCodec::loadFromSafetensors` loads the **encoder** block too
  (`Codec/SA3Codec.cpp:133-140`, 158-159). That is 1.70 GB of F32 in the
  medium checkpoint (234 tensors), and generation never uses it: `decode` only
  touches `decoderBlock`.
- The DiT weights (5.8 GB) and the T5 encoder are still alive through decode.
  Everything lives in `main`'s scope (`Main.cpp:131,141`).
- T5: the 1.18 GB file also holds the decoder stack and a second 393 MB
  embedding table, but only the rows it touches become resident. Fine.

**Fix, cheapest first.**
1. Scope the checkpoint `SafetensorsFile` so it unmaps as soon as the DiT and
   codec are loaded. `readSingleF32TensorFromSafetensors` already re-opens the
   file with an `ifstream` for the padding embedding. About −9 GB RSS.
2. Add a decoder-only codec load for generation (the encoder stays available for
   tests and `encode`). −1.7 GB GPU and RSS, and ~−0.2 s load.
3. Destroy `weights` and `textEncoder` before `codec.decode`. −5.8 GB peak
   during decode.
4. Later: zero-copy. `Buffer` already has an adoption path
   (`Buffer-Apple.mm:67-95`, `newBufferWithBytesNoCopy`), but it requires
   page-aligned memory and **no tensor in either checkpoint is page-aligned**
   (all are 4-byte aligned). The route is to adopt the whole mapping as one
   buffer and give `Tensor` a byte offset, binding through the existing
   `BufferRange`. That removes the 0.76 s + 0.49 s copies as well. The
   downsides: `Tensor` API churn, a Metal buffer over a `PROT_READ`
   `MAP_PRIVATE` mapping has to be proven, and WNConv1d / T5 weights are
   transformed on the host, so they can't be adopted.

**Expected win.** Peak RSS 19.3 GB → ~8–9 GB with 1–3. Load −0.2 s. With 4,
load time −1.2 s and RSS ≈ GPU working set.

**Risk.** 1–3 are **bit-exact**: same bytes, same buffers. 4 is **bit-exact**
but medium effort.

---

### 4. Kernel / pipeline caching — and the 2.5 s warm-up

**Evidence.** Every op follows the same pattern: `auto kernel = XKernel {};
… kernel.prepare(device); kernel.dispatch(…)`. Examples: `Linear.cpp:296-310`,
`Attention.cpp:245-271`, all of `Codec/GpuOps.cpp`, `Sampler/Ops.cpp:40-45`.
Each call pays:
1. `compile()` — graph build and MSL emission (measured above: 221 µs for
   `LinearF32` in Release).
2. `prepare()` → `newLibraryWithSource` + `newComputePipelineStateWithFunction`
   (`GPU/Shader/ShaderLibrary-Apple.mm:26`,
   `GPU/Pipeline/ComputePipeline-Apple.mm:31`). No eacp-side cache exists. After
   the first time, Metal's own cache makes these lookups rather than compiles,
   but they are still a synchronous call per dispatch.

The first step of sampling pays the real MSL compiles for ~20 distinct DiT
kernels, `LinearF32` being large, with the GPU idle because `commit()` blocks.
That is the likely cause of the 97%/0% alternation in the first ~2.5 s. A
second cause to check: the first command buffer that touches 9.5 GB of new
weights also has to make them resident.

**Fix.** A per-`Device` cache of prepared kernel instances keyed by kernel type,
plus the specialisation for kernels whose source depends on constructor
arguments. For example, `sharedKernel<LinearF32>(device)` returns a prepared
object that call sites set members on and dispatch. This is safe because
`pass.dispatch` binds buffers and `setBytes` the uniforms at encode time. One
thing to check: whether a bound `Uniform<InputBuffer>` member holds a `Buffer`
by value, keeping the last temporary alive. If it does, clear it after the
dispatch. Then warm the cache during load, on a worker thread if `Device`
allows it (it asserts an owning thread for buffer I/O). For zero cold-start
across runs, persist the pipelines in an `MTLBinaryArchive`.

**Expected win.** ~50 ms+/step of encode in Release (more at -O0), and the
first-step compile moves into load, where it can overlap weight loading.
Sampling −0.5 to −2 s. The same change also speeds up decode and T5 encode.

**Risk.** **Bit-exact** (identical MSL). Small-to-medium effort: a mechanical
edit across ~35 call sites. `RESULTS.md` already names this as the known gap.

---

### 5. Per-step temporaries: a fresh MTLBuffer per op, all live until commit

**Evidence.** `Tensor::uninitializedF32` → `Device::makeBuffer` →
`newBufferWithLength` for every op output (`Tensor.cpp:56-63`,
`Buffer-Apple.mm:53-55`). A medium DiT step allocates ~1,450 buffers. The whole
step is one command buffer (`Sampler/Sampler.cpp:56-67`), and Metal retains
every buffer a command buffer references until it completes, so every
intermediate of all 24 layers is alive at once. Per layer that's roughly
qkv 10 MB, five slices, four per-head norms, four RoPEs, two 11 MB `scores`,
the FF hidden state and so on — on the order of 100–200 MB. ×24 ≈ 3–5 GB, which
matches the 7↔12 GB swing. Fresh shared-storage pages must be zero-filled and
made GPU-resident when the command buffer is submitted, then unmapped when it is
freed. That is CPU/kernel work the GPU waits on. In decode, at ~6 GB per layer
before item 2, it is the best candidate for the 1.2 s idle per layer; confirm
with Metal System Trace.

**Fix.** Put a size-bucketed buffer pool behind `Tensor::uninitializedF32`: a
`Tensor`'s destructor returns the `Buffer` to the pool, and the next allocation
of that size takes it. Reusing a buffer *inside* the same command buffer is
safe on a serial compute encoder, because Metal's hazard tracking orders the
reuse write after the earlier reads. `zeroPadRowsGpu` and friends already zero
explicitly (`GpuOps.cpp:401-423`). Audit that no kernel relies on
`newBufferWithLength` returning zeros; the name `uninitializedF32` says none
should.

**Expected win.** GPU memory swing gone (peak ≈ weights + one layer's working
set). Sampling −10–30%, but that range is a guess; measure it. It also removes
decode's allocation stalls on the paths item 2 doesn't cover.

**Risk.** **Bit-exact** if the zero-fill audit is clean. Medium effort (pool
lifetime vs. `Device`, thread ownership).

---

### 6. Stop serialising CPU and GPU: async commit

**Evidence.** `commit()` = `commit` + `waitUntilCompleted`
(`CommandBuffer-Apple.mm:131-134`). The sampler commits and waits every step
(`Sampler.cpp:67`). The decoder commits and waits 12 + 3 times
(`TransformerResamplingBlock.cpp:67-90,132-189`) and once more in
`SameCodec::decode` (`SA3Codec.cpp:194-202`). The CPU encodes while the GPU
idles, then the GPU runs while the CPU idles.

**Fix.** Use `commitAsync()` for steps 0…n−2 and wait only at the end: the next
step's input is a GPU buffer, and queue order handles the dependency. The
per-step `uploadNoise` makes a new buffer, so it doesn't wait. In decode, merge
into one command buffer.

**Expected win.** The per-step encode time is hidden behind the GPU: after
items 1 and 4 that's small (~0.1–0.5 s total), but it's free.

**Risk.** **Bit-exact**. Small effort. Combine it with item 5 carefully: a
pooled buffer can't go back to the pool while an *uncompleted earlier* command
buffer still reads it. Either tag pool entries with the command buffer that
last used them, or keep the pool per command buffer.

**Decision: skipped.** Measured once the buffer pool (item 5) was in: a medium
step spends ~1 ms encoding and ~0.4 ms making its noise, against ~215 ms of
kernels. Overlapping steps could save at most ~1.5 ms a step, ~12 ms over 8
steps, while keeping two steps' temporaries alive at once. The library already
has the pieces for a caller that does need it — `CommandBuffer::submit()` and
`wait()`, with a pipelined loop in the GPU README — so nothing is missing from
eacp, and the sampler stays synchronous.

---

### 7. Hoist the constant cross-attention K/V out of the step loop

**Evidence.** The context is the same for all 8 steps, but every step
recomputes `projectedContext` (`DiT/SA3DiT.cpp:354-356`). Every layer then
recomputes `kv2Full = linear(context, crossAttnKVWeight)` (256 × 1536 → 4608),
three slices, and two per-head RMS norms (`SA3DiT.cpp:227-251`). That's
~3.6 GFLOP × 24 ≈ 87 GFLOP a step, about 9% of the ~1 TFLOP a medium step does,
plus ~170 dispatches.

**Fix.** Before sampling, compute per layer `{k2n, k2DiffN, v2}` once (with the
non-differential variant for small), and pass them into `forward`.

**Expected win.** Sampling −~8%, about −0.7 s at the current rate.

**Risk.** **Bit-exact**: the same kernels on the same inputs. Small-to-medium
effort (signature change through `forward` / `transformerBlock`, with tests
updated).

---

### 8. Drop the host-built all-zero attention masks

**Evidence.** A null mask makes `attention()` build a zero mask **on the CPU** —
`std::vector<float>(rows*cols)` plus `newBufferWithBytes`
(`Attention.cpp:183-187,234-236`) — and the scores kernel then reads it
(`Attention.cpp:75`). The DiT passes `nullptr` for all four attention calls
per layer (`SA3DiT.cpp:140,166,253,264`). That's 96 host allocations and copies
per step (339×339 and 339×256 floats, ~40 MB/step). The SAME-S chunked path
does the same per chunk per layer (`CodecTransformerBlock.cpp:137-139`).

**Fix.** A mask-free variant of `AttentionScoresKernel` (the mask term compiled
out).

**Expected win.** Small in time (tens of ms/step), and it removes 96 host
allocations per step.

**Risk.** **Bit-exact**: `x + 0.0f == x`, apart from `−0 → +0`, which feeds
`exp` identically. Small effort.

---

### 9. Attention kernels: 64× redundant `exp`

**Evidence.** `AttentionWeightedSumKernel` runs one thread per (row, head, d),
and each thread recomputes `exp(score − peak)` for every column
(`Attention.cpp:157-167`). Every probability is computed `headDim` = 64 times.
`AttentionScoresKernel` does a serial 64-wide dot product per thread with no
tiling.

**Fix.** Bit-exact half: have the stats pass (or a separate pass) write
`p = exp(s − peak)` back over `scores` once, and have the weighted sum read `p`
in the same column order. That's the same `exp` inputs and the same
accumulation order. Tolerance half: a fused online-softmax (flash-style) kernel
with simdgroup matrices changes the summation order.

**Expected win.** After item 2 the codec attention is small. The DiT attention
(339 rows) and T5 (256 rows) are a minor share of their phases. The bit-exact
half is cheap; the flash kernel only earns its effort for long sequences
(SAME-S chunks don't need it).

**Risk.** Precompute-`p`: **bit-exact**. Flash: **tolerance**.

---

### 10. `LinearF32` efficiency — the real floor for sampling and decode

**Evidence.** A medium step is ~2 × 339 rows × 1.43 B layer params ≈ 1 TFLOP.
At 1.17 s/step that's under 1 TFLOPS effective, against a fp32 peak an order of
magnitude higher. The kernel (`Linear.cpp:45-142`) uses 64×64 tiles and 32-deep
slabs. Each thread does 16 scalar global loads per slab, each behind a
`min`/`select`, with two barriers per slab and no double buffering. Bias is a
second full read/write pass (`AddBiasRows`, `Linear.cpp:313-320`). The q/k/v
column slices are separate copy kernels, 5 per layer
(`CodecTransformerBlock.cpp:115-119`, `SA3DiT.cpp:128-130,155-156`).

**Fix.**
- Vectorised (`float4`) global loads with the bounds clamp hoisted to edge tiles
  only.
- Double-buffered slabs.
- Bias fused into the epilogue.
- Strided reads in place of `sliceColumns` copies.

Profile first: after items 1, 4, 5 and 6, the Metal trace will say how much of
the step is `LinearF32`.

**Expected win.** Plausibly 2–3× on the linears, which are most of sampling and
of the post-item-2 decode. Wall −3 to −6 s.

**Risk.** **Bit-exact** as long as the per-element k accumulation order is kept
(tile shape in M/N and load strategy don't change it; the simdgroup MMA
sequence over `kk` must stay the same). Fused bias is bit-exact (the same single
add). Changing the slab depth or splitting K means **tolerance**. Medium-large
effort.

---

### 11. SAME-S (small model) decode: one dispatch chain per 34-row chunk

**Evidence.** `runChunkedStack` loops over chunks and runs every layer on a
34-row slice (`TransformerResamplingBlock.cpp:44-53`). A 12 s clip has ~65
chunks × 6 layers × ~40 tiny dispatches, plus a host zero mask per chunk per
layer. Not on the medium path, but it's why small decode is slow.

**Fix.** Run all chunks through each layer at once. Every op except attention is
row-wise, and attention restricted to the row's own chunk is a block-diagonal
band — the kernel from item 2 with `[chunkStart, chunkEnd)` limits.

**Risk.** **Bit-exact** if the lane mapping uses the column's index within its
chunk (which is what the per-chunk run does today). Row-wise `linear` is
bit-exact regardless of M. Medium effort, and most of it is shared with item 2.

---

### 12. Load-time overlap and host conversions

**Evidence.**
- T5 weights are bf16 and are widened on the CPU one element at a time with a
  `memcpy` per element (`T5GemmaEncoder.cpp:103-128`).
- `gatherEmbeddings` and `buildPaddingMask` are host-built (`:160-213`).
- The tokenizer parse is pure CPU and independent of the DiT load.

**Fix.**
- Parse the tokenizer (and widen the bf16) on a worker thread while the main
  thread loads the DiT, since only `makeBuffer` needs the owning thread.
- Or cache a compact binary vocab/merges file next to `tokenizer.json`.

**Expected win.** ~0.3–0.5 s after item 1. On -O0 it would be several seconds.

**Risk.** **Bit-exact**: bf16→f32 is exact. Small effort.

---

### 13. Half-precision DiT weights (later, numerics-sensitive)

**Evidence.** `LinearPackedHalf` and `loadPackedF16` already exist
(`Linear.cpp:144-261`, `SafetensorsFile.cpp:144-171`). The DiT is 5.7 GB of F32.

**Fix / win.** Halves DiT weight memory and bandwidth. Doubles simdgroup-matrix
throughput if the kernel moves to half MMA.

**Risk.** **Tolerance**, and it can fail: fp16 weights move outputs by more
than rounding. Only worth doing after items 1–10, and only if it passes the 3e-2
medium forward golden and a listening check.

---

## Rough end state

The items don't add up exactly, but here is where things should land.

| Phase | Now (-O0) | After 1–8 |
|---|---|---|
| Load (DiT + codec + T5) | 6.7 s | ~1.5 s (~0.3 s more with zero-copy) |
| Sampling | 9.4 s | ~6–7 s (~3 s more with item 10) |
| Decode | 22.0 s | ~3–5 s |
| Wall | 38.6 s | ~12 s |
| Peak RSS | 19.3 GB | ~8–9 GB |

Suggested order: 1 → 3 (1–3) → 2 → 4 → 6 → 5 → 7 → 8, then profile before
10 / 11. Items 1, 3, 4, 6, 7 and 8 are bit-exact by construction. 2, 5 and 11
are bit-exact with the care noted. Only 9-flash, 10-with-K-changes and 13 need
the goldens' tolerance.
