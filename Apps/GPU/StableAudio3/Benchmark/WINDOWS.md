# StableAudio3 on Windows / D3D12

State of the Windows half of the perf work: what moved, what was wrong and why
those bugs are the reusable part, and what is left. Numbers are
an RTX 6000 Ada (48 GB, driver 528.49), Windows 11 26200, MSVC 19.51,
RelWithDebInfo, `EACP_UNITY_BUILD=OFF`.

## Where it got to

All verified producing real audio, and bit-identical to the run before unless
a line says otherwise.

| | at `c8b78c38` | now |
|---|---|---|
| small 12 s, total | 11.30 s | 2.86 s (2.75 s Release) |
| — sampling | 3.48 s | 0.61 s (0.46 s Release) |
| — decode | 4.72 s | 0.26 s |
| — text encoder load | 1.13 s | 0.32 s |
| medium 30 s, total | silent output | 7.34 s |
| `LinearF32`, one small DiT step | 39.97 ms | 14.24 ms |
| `MLTests`, cold → warm shader cache | 4.11 s | 0.39 s |

Release and RelWithDebInfo differ less than they look: the tokenizer parse is
0.903 s against 0.900 s, identical. Sampling is the phase that gains.

`LinearF32` is still the largest single item at ~70% of a small DiT step.

## Four things that were wrong, and are the reusable part

**A dispatch dimension has a ceiling off Metal.** D3D12 and Vulkan cap
threadgroups at 65535 per dimension; Metal does not. Worse, what an over-sized
dimension does is the driver's business: this NVIDIA one runs an X grid far
past the cap and produces *nothing at all* for a Y past it — no error, no
removed device. Medium above ~15 s decoded to digital silence because
`rows * heads` was one dimension and crossed 65535 at 161 latent frames. Rows
and heads now take a dimension each. See `Lib/eacp/GPU/README.md`.

**Two naming calls either side of a `+` is a coin flip.** `nameOperandsFirst`
returned `define(...) + holdTheRecord(...)`; both hand out local names, and the
operands of `+` are unsequenced, so clang and MSVC emitted *different shaders
from the same graph*. Windows had been emitting redundant buffer reads, and CI
could not catch it because the Windows lanes build the emitter with MSVC while
the goldens were written from a clang build.

**Off Metal, a SIMD-group matrix product was barrier-bound.** Metal has the
instruction; everything else staged both operands through threadgroup scratch —
two whole-group barriers and 28 shared accesses per 8×8×8 product. `LinearF32`
paid over a hundred barriers per K-slab and ran at ~3% of this card's fp32
peak. It now reads operands where they already lie when nothing has moved them,
which is bit-exact and 2.6× faster.

**A committed D3D12 resource costs ~246 µs**, against single-digit microseconds
for a Metal buffer, and the ML layer allocated one per intermediate tensor —
5527 buffers and 1.36 s of pure allocator time in one codec round trip. Fixed
upstream by the shared buffer pool. `EACP_BUFFER_STATS=1` still counts buffer
creations and fence waits and prints totals at exit; it is how to check this
has not come back.

## Environment notes

- **`GPUTests` fails 94 cases on this machine**, identically on `main` and on
  this branch, and identically on hardware and under `EACP_D3D12_WARP=1`. All
  render/texture suites (Stencil, Scissor, TextureRegion, CubeTexture,
  MultisampledTarget…) that SA3 never touches. CI is green on a GitHub runner,
  so this is the box, not the code. Parked deliberately — do not read a
  non-zero failure count here as a regression without diffing the suite names.
- **The driver is 528.49 (Jan 2023)**, old for an Ada card. It is not the cause
  of anything above — every failure reproduced identically on WARP — but the
  headline numbers are worth retaking on a current driver.
- `--profile` prints per-kernel GPU time for one DiT step; it is the fastest way
  to see where a step goes.
- The disk shader cache matters far more here than on Metal, because FXC
  compiles from source every run and nothing underneath remembers. Delete
  `%LOCALAPPDATA%\<app>\Shaders` to get a genuine cold measurement.

## The pool's retention window, and what it is and is not worth

`BufferPool` freed storage nobody asked for again after two submissions.
A sampling step submits once, so its temporaries survived into the next step
and were reused. A codec decode submits four to six times, so every temporary
it made was freed before the next decode asked for that size: **181 committed
resources and 1.5 GB created per decode, every decode, for no reuse at all** -
82 ms of a 140 ms decode on D3D12, where each one costs about 450 us. Sixty-four
submissions outlives a round of anything here, and peak GPU memory is unchanged
(12427 MB against 12422 MB over a medium 30 s run), because what the pool now
holds is exactly what was being freed and reallocated a moment later.

**What it is worth: a second decode in the same process goes 0.14 s to 0.02 s**,
which is PyTorch-CUDA's 0.022 s. So what looked like a six-fold kernel gap in
the codec was the pool and not the kernels.

**What it is not worth: anything at all to a one-shot CLI run.** The app's own
phase total falls (2.62 s to 2.51 s on small 12 s) and the wall clock does not
move - 2.75-2.82 s before, 2.75-2.89 s after, measured three runs each way.
The saving is spent releasing the buffers the pool held when the process exits.
Quote the wall clock, not the phase total, for anything single-shot; the
steady-state number is the one that matters to a server, a plugin or a UI that
generates more than once.

## Measured and rejected

Four things that looked like wins and were not, kept here so nobody spends the
afternoon twice.

**bf16 weights.** Halving the weight bytes would halve the load, and bf16 to
fp32 is exact, so it looked bit-exact by construction. It is not available:
`model.safetensors` stores **F32**, 997 tensors and 9.22 GB on medium, 685 and
2.27 GB on small. Only T5Gemma is BF16 (340 tensors, 1.18 GB), and its load is
0.34 s and already overlapped. Converting the F32 weights down would be a real
precision change, not a widening.

**Merging a run of products into one loop.** With the barriers gone, a 4x4
block of fragment products reads each operand element four times, so emitting
the whole block as one loop with the reads named once should have cut
threadgroup traffic fourfold. Measured: `LinearF32` 15.43 ms to 16.85-17.44 ms,
consistently worse. FXC already shares those reads; the named temporaries only
added register pressure. Reverted. The emitter should not try to out-guess the
shader compiler on common subexpressions.

**D3D12_HEAP_FLAG_CREATE_NOT_ZEROED.** Kept, but it is worth ~8% rather than
the ~100% of the clearing cost the arithmetic suggests: Windows must clear a
page the first time it hands it to a process whatever the flag says, so on a
cold run it only saves re-clearing pages the process already owns.

**A compact tokenizer cache.** Once the parse runs beside the weight load it is
entirely hidden - 0.92 s of parse inside a 1.05 s load - so caching it would
save real CPU and no wall clock. Worth doing only if the weight load ever gets
fast enough to expose it.

## Where the load time actually goes

Medium asks for **2616 buffers and 13477 MB**, and 1.09-1.19 s of the load is
inside `CreateCommittedResource` alone, about 11 GB/s, which is memory
bandwidth for committing and clearing rather than per-call overhead - so fewer
and larger allocations would not help. The remaining ~1.3 s is the CPU copy
from the mapping into the upload arena. The lever nobody has pulled is
`ID3D12Device3::OpenExistingHeapFromAddress` over the whole mapped file, which
would let the GPU copy straight out of the page cache and remove that CPU copy
entirely.

## Open, in the order I would take them

1. **Hand-written HLSL that bypasses `ShaderLibrary`** does not go through the
   disk cache. `GPUTests` creates no cache directory at all for that reason.
   Route those paths through the same cached compile.
2. **`LinearF32` itself**, still ~70% of a step. The barriers are gone, so what
   is left is the memory system: vectorised loads with the bounds clamp hoisted
   to edge tiles, double-buffered slabs, bias fused into the epilogue.
3. **The dead `float2`** a fused operand still loads. Deliberately not built: it
   needs a two-pass emit to know every use was fused, and there is no evidence
   yet it costs anything — the shader compiler can see it is dead. Measure
   before building.
4. **PyTorch-CUDA vs eacp-D3D12** on this card, the way `RESULTS.md` has Metal
   vs MPS. `benchmark.py` takes `--model/--seconds/--steps/--seed/--prompt/
   --eacp-binary` and `SA3_PYTORCH_REPO`; it needs a venv with torch cu126.
   Never run here.
5. **DXC/SM6** is in, behind `EACP_D3D12_DXC=1`, deliberately opt-in: it is not
   bit-exact against FXC (up to 29/32768 on a sample) and gating it on whether
   a DLL is installed would make one binary produce different audio on two
   machines. Worth ~5% today; the reason to keep it is that wave intrinsics and
   16-bit types are unreachable from `cs_5_0`. If it ever becomes the default,
   re-bless the goldens from a DXC build first.

## Building here

```bash
cmake -G Ninja -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEACP_UNITY_BUILD=OFF
cmake --build build-rel --target StableAudio3
```

from a shell with `vcvars64.bat` sourced. Checkpoints land under
`%APPDATA%\StableAudio3\Resources\huggingface`, and the HF token is read from
`%USERPROFILE%\.cache\huggingface\token`.
