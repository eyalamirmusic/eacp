# Plan: EDSL-CoreML

A Core ML backend for the compute the shader EDSL already expresses at the
tensor level, so a net written once against eacp runs on Metal, D3D12 and
Vulkan through the compute kernels it runs on today, and on the Apple Neural
Engine through Core ML. The net that proves it is
[WhisperEACP](https://github.com/eyalamirmusic/WhisperEACP): its encoder is
the first thing to run both ways, its test suite is what says the two ways
agree, and its benchmark is what says the second way was worth having.

## What this is not

It is not a fourth backend of `GPU::ComputePass`. A `ComputeProgram` body is
one thread's view: `threadId()`, `shared<>`, `barrier()`, `atomicAdd`,
`simdMatrix`, a `loop` with a data-dependent exit, a `write` at a computed
index. Core ML has no thread. It runs a fixed graph of whole-tensor ops that
someone compiled ahead of time, on whichever of the CPU, GPU and Neural Engine
it decides to. The kernels WhisperEACP is written out of stay exactly what
they are and keep serving every platform; nothing lifts them.

It is not Core AI either. Core AI (WWDC26, the 27 OSes) only consumes
`.aimodel` bundles, only the Python packages produce those, and the API is
Swift only. Nothing authored on the device at run time can reach it, and
WhisperEACP fetches its model at run time and builds everything from the
safetensors on the machine. Core ML's ML Program format is a public protobuf
that an ObjC++ process can write and compile, which is the whole reason it is
the target. If Core AI ever ingests a MIL program, the builder below gains a
second consumer behind the same seam.

## Where the sharing happens

The shared code is the net, not the kernels. `Encoder::encode` in WhisperEACP
already reads as a tensor program once the dispatch details are taken out of
it: unfold, a linear with GELU on the store, add the positional embedding,
then per layer a layer norm, three projections, scores, apply, the out
projection with the residual, a layer norm, fc1 with GELU, fc2 with the
residual. That sequence is op for op what a MIL program of the encoder holds.

What is backend-specific in that file is which kernel serves each op and how
it is dispatched: the tiled against the split projection, the packed-half
twins, the zero-bias buffer, the tile maxima that fold the softmax into the
apply, the split counts. Those move out into a backend. The sequence stays
behind, written once against a seam of ten ops:

```cpp
struct Net
{
    virtual Tensor linear(Tensor input, const Weight& weight, const Weight& bias,
                          Fused fused) = 0;               // none, gelu, residual
    virtual Tensor layerNorm(Tensor input, const Weight& weight,
                             const Weight& bias) = 0;
    virtual Tensor conv1d(Tensor input, const Weight& weight, const Weight& bias,
                          int stride, Fused fused) = 0;
    virtual Tensor add(Tensor a, Tensor b) = 0;
    virtual Tensor attention(Tensor queries, Tensor keys, Tensor values,
                             int heads, bool causal) = 0;
    virtual Tensor softmax(Tensor rows) = 0;
    virtual Tensor embed(Tensor tokens, const Weight& table, int position) = 0;
    virtual Tensor argmax(Tensor logits, const Mask& suppressed) = 0;
    virtual Tensor cacheAppend(Cache& cache, Tensor rows) = 0;
    virtual Tensor transpose(Tensor rows) = 0;
};
```

The ops sit at that level, above matmul and softmax, on purpose. The kernel
backend fuses the softmax into the apply and the GELU into the store; the Core
ML backend lowers `attention` to the fused `scaled_dot_product_attention` op
the Neural Engine handles as one. Ops at the matmul level would take that
freedom from both. The exact list is settled when the kernel backend is
extracted (phase 2), because that extraction is what shows which calls the
encoder and decoder really make.

Two backends:

- **The kernel backend** is today's code moved one file over. A `Tensor` is a
  `GPU::Buffer` plus a shape, `linear` picks the tiled or split program by row
  count and weight storage, and every `pass.barrier()` stays where it is. The
  three GPU platforms keep working unchanged and the tests keep their
  bit-exact references.
- **The Core ML backend** records the same calls into a MIL program at
  prepare time, writes the weights into the model's blob, compiles it once
  through Core ML, caches the compiled model, and at run time makes one
  prediction. A `Tensor` there is a MIL variable.

Against the kernel backend `encode()` runs per window, as now. Against the Core
ML backend it runs once at prepare and its result is a model.

## Encoder first; the decoder stays on the GPU

The same split whisper.cpp made for its Core ML support, for the same reasons:

- The encoder is one fixed-shape graph, 1500 x 384 for tiny.en, dominated by
  matmuls. That is the Neural Engine's best case and nothing in it falls back.
- A decode step is one token through ninety small ops, and the GPU path is
  already tuned for exactly that: device-side argmax, the sampled token fed to
  the next step through a buffer slot with the host nowhere in between, steps
  in flight. Every Core ML prediction is a host round trip of a few hundred
  microseconds plus the call, so for tiny.en the engine loses that race. A KV
  cache on Core ML is possible, with `MLState` (macOS 15) and a fixed cache
  length behind a mask, which is how WhisperKit does it. It is phase 4, and
  only if phase 3's numbers say so.
- The seam between the two halves is one buffer of encoder rows, which the
  decoder already takes as a `Buffer`. The Core ML encoder's output is copied
  into one: a memcpy into shared storage on Metal, 2.3 MB per window.

The mel front end stays on the GPU too. It is kernels, it is cheap, and its
output crosses the same seam the other way.

The payoff is specific to the live transcriber: the encoder runs on the Neural
Engine while the GPU decodes the previous window and draws the UI. Today the
three contend for one device.

## What lands in eacp

A new module, `Lib/eacp/ML`, Apple only, behind a seventh capability variable
`EACP_HAS_COREML` beside the six in the top-level `CMakeLists.txt`, on where
`EACP_HAS_GPU` is and the platform is Apple. Everything Core ML stays inside
it: no header an app includes names an `MLModel`.

### `ML/Graph` - the program builder

The tensor-level EDSL. `Tensor` handles with the value operators the shader
EDSL already has (`+`, `*`, `exp`, `select`, comparisons, uniforms as scalar
inputs), plus the axis-level ops the shader EDSL cannot spell: `matmul`,
`transpose`, `reshape`, `softmax(axis)`, `sum(axis)`, `max(axis)`, `layerNorm`,
`conv`, `gather`, `concat`, `slice`, `scaledDotProductAttention`. An
elementwise expression on tensors reuses the existing `ShaderGraph` node kinds
as an "apply per element" op, so the value layer is shared rather than
duplicated. Shapes are fixed at `prepare()`; a graph may declare a list of
enumerated shapes for an input, and no more than that (see shapes, below).

The Whisper `Net` ops are not here. They belong to the net that has them.
What is here is what those ops lower to.

### `ML/MIL` - the emitter

A writer for Core ML's ML Program format: `Model.proto` and `MIL.proto` from
coremltools' `mlmodel/format`, encoded by a small hand-rolled protobuf writer
so the runtime pulls in no protobuf library, and the `.mlpackage` directory
around it (`Manifest.json`, `Data/com.apple.CoreML/model.mlmodel`,
`Data/com.apple.CoreML/weights/weight.bin`). Weights go into the blob file,
fp16 or fp32 as the safetensors shipped them, in the layout coremltools'
`MILBlob` writer uses, which is the one Core ML reads. The spike (phase 0)
decides whether small constants go inline in the program instead.

An emitter regression is caught the way the GLSL ones are: every program the
tests emit is compiled by Core ML inside the suite, on the macOS lane.

### `ML/Model` - the runner

- `compile(package) -> compiled model`, through `MLModel compileModelAtURL:`,
  cached under `FilePath::appCacheDirectory() / "CoreML"` keyed by a hash of
  the program bytes and the weights' identity, the way the Vulkan pipeline
  cache is kept under `$XDG_CACHE_HOME/eacp/`. Core ML keeps its own Neural
  Engine compilation cache beside that; the first load of a new model pays
  seconds, later loads do not.
- `ComputeUnits` as an option: all, CPU and Neural Engine, CPU and GPU, CPU.
  Default all.
- `predict()` returning `Threads::Async<void>`, resolved on the main thread,
  and a blocking form for a console app, the shape `OnlineResource` has.
- Inputs and outputs as `ML::Array`: an `MLMultiArray` over an IOSurface-backed
  pixel buffer in `OneComponent16Half` where the tensor is fp16, since that is
  the one form the Neural Engine reads and writes without a copy, and a plain
  `MLMultiArray` otherwise. `Array::copyTo(GPU::Buffer&)` and `copyFrom` are
  the seam to the kernel path.
- `computePlan()`, `MLComputePlan` (macOS 14.4) read back as one entry per op
  with the device it landed on and its cost estimate, so a test can assert an
  encoder went to the Neural Engine rather than silently to the GPU, and so
  `DeviceInfo` can print it.

Minimum OS: macOS 14 for the module, 14.4 for the plan, 15 for state. iOS
gets the same module; the simulator runs CPU only and the tests self-skip
there as the GPU ones do.

### `Tests/ML`

Graph tests with no model: a handful of small programs (elementwise chain,
matmul, softmax over an axis, layer norm, one attention block) built through
the builder, compiled by Core ML in the suite, run on the CPU and the Neural
Engine, and checked against a scalar reference with a measured fp16
tolerance. `EACP_REQUIRE_ANE=1` makes the engine-placement assertions fail
rather than skip, as `EACP_REQUIRE_GPU=1` does for the device suites, so the
macOS CI lane, which has one, runs them for real.

### `Apps/ML`

One console app that builds a matmul-and-softmax program at run time,
compiles it, prints the compute plan and times compile and prediction at a
few sizes: the spike, kept as the worked example.

## What lands in WhisperEACP

On a branch of the same name, `EDSL-CoreML`, configured against this tree with
`-DCPM_eacp_SOURCE=$HOME/Code/eacp` until the eacp side is pushed and
`develop` carries it. Its `CLAUDE.md` says that override is for exactly this
case: an eacp change made alongside the WhisperEACP change that needs it.

- `Lib/WhisperEACP/Net/`: the `Net` seam, the kernel backend extracted from
  `Encoder.cpp` and `Decoder.cpp`, and the Core ML backend over `eacp::ML`.
  `Encoder::encode` and the decoder's two recording calls become the shared
  bodies. `Whisper::prepare` takes a backend choice: kernels, or Core ML for
  the encoder.
- `Tests/Net`: the encoder run both ways over the same mel, compared row by
  row at the tolerance phase 3 measures; the compute plan asserted to have
  placed the encoder on the Neural Engine under `EACP_REQUIRE_ANE=1`.
- `Tests/Oracle`: the transcript oracle gains the Core ML encoder as a second
  contestant. Same tokens on jfk.wav is the bar.
- `Benchmark`: a fourth column, `WhisperEACP ANE`, the encoder on Core ML and
  the decoder on Metal, beside the existing three, and a `--live` row for it.
  The header names which compute units the model was compiled for and what
  the plan reports.
- `Apps/Console/DeviceInfo` prints whether a Neural Engine is present and what
  the plan says of the encoder.

Every phase below ends with WhisperEACP's full suite green on Metal and on
Windows, since nothing in the kernel path is allowed to change behaviour, and
with the benchmark rerun so a regression on the GPU side is caught the day it
lands rather than after.

## Phases

### Phase 0 - the spike

One `.mm` file, no library yet. Hand-write a MIL program for a
`[1500, 384] x [384, 384]` matmul followed by a softmax over rows, weights in
the blob, compile it at run time, predict, read the compute plan.

Done when these numbers are written down in this file, on an M-series Mac:

| question | answer |
| --- | --- |
| time to write and compile the package, first and second time | |
| prediction latency at that shape, CPU-and-engine versus all | |
| did the two ops land on the Neural Engine | |
| max abs and rel error against an fp32 CPU reference | |
| blob weights against inline constants: does either matter | |

The spike also settles the two format questions that documentation does not:
the exact blob layout Core ML accepts, and whether an enumerated-shape input
keeps a matmul on the engine.

### Phase 1 - the eacp module

`ML/MIL`, `ML/Model`, `ML/Graph` with the op set the Whisper encoder needs and
no more, `Tests/ML`, `Apps/ML`, the capability variable, the README table row.
Done when `Tests/ML` passes with `EACP_REQUIRE_ANE=1` on a Mac with an engine
and self-skips cleanly on one without, and the module builds for iOS.

### Phase 2 - the seam, with no behaviour change

In WhisperEACP: `Net`, the kernel backend, the encoder and decoder rewritten
against it. Done when every existing test passes unchanged, the encoder and
decoder outputs are bit identical to before (the oracle tests are the check),
and the benchmark's `WhisperEACP` column is within noise of the number before
the refactor. This phase is the one that decides the final op list, because it
is where the real calls are counted.

### Phase 3 - the Core ML encoder

The Core ML `Net` backend, the encoder built through it at prepare, the
enumerated audio-context shapes, the transpose on the output, the seam copies,
`Tests/Net`, the oracle contestant, the benchmark column, the live transcriber
running the encoder on the engine. Done when the transcript matches on the
test audio, the row-by-row tolerance is measured and written down, and the
benchmark shows the encoder's wall time on the engine beside its Metal time,
with the GPU idle during it.

### Phase 4 - the decoder, if the numbers say so

Only if phase 3's per-prediction overhead, measured, leaves room for a
decode step to win. Stateful model with `MLState` for the KV cache, fixed
`maxPositions` with a mask, the argmax inside the model, the token carried as
state. Not planned in detail until phase 3 reports.

## Decisions taken now

- **Precision.** The Neural Engine is fp16 end to end. The Core ML encoder
  will not match the fp32 kernel path bit for bit, and the tests do not
  pretend it will: they compare at a tolerance phase 3 measures, and the
  transcript oracle is the acceptance test. Layer norm is where fp16 encoders
  drift first, so the tolerance is measured per stage, not once at the end.
- **Shapes.** The engine wants static shapes. The live path runs the encoder
  over fewer than 1500 positions (`Whisper::audioContextForSamples`, tiles of
  64 from a floor of 448), so the program declares that set as enumerated
  shapes, one compiled program per member, and a context outside the set
  rounds up to the next member. If the spike shows enumerated shapes push the
  matmuls off the engine, it is one model per used context instead.
- **Layout.** The engine wants channels on the second axis, the transpose of
  the frame-major rows the decoder reads. The transpose is the last op of the
  MIL program, so the copy across the seam is a plain memcpy.
- **Weights.** The Core ML path takes them straight from the safetensors into
  the blob at the storage they shipped in. The kernel path keeps its own
  upload. A model is therefore built and compiled once per machine per model
  version, and the cache key says so.
- **Threads.** Core ML predictions run on their own queue; `predict()`
  resolves on the message thread through `Threads::callAsync` like every
  other Async in eacp. The live transcriber's encoder call becomes an Async
  rather than a commit-and-wait.
- **Placement is asserted, not assumed.** A test that wants the engine reads
  the plan. A build that cannot get it fails under `EACP_REQUIRE_ANE=1` and
  skips otherwise.

## Risks

- The blob format and the ML Program spec are read out of coremltools rather
  than a document. The spike is where a wrong guess costs a day rather than a
  phase.
- Enumerated shapes on the engine are the one behaviour Apple documents least.
  The fallback, one model per context, is a cache-size cost and nothing else.
- A per-prediction floor of a few hundred microseconds is fine for a 30 s
  window and is what rules the decoder out until measured.
- Core AI may become the only way to reach new engine features. The seam is
  the insurance: nothing above `Net` knows which Apple framework is under it.

## Gaps for eacp, as they surface

The section WhisperEACP's own plan keeps, kept here for the same reason: a gap
belongs in eacp, not in a workaround downstream. Empty until phase 2.
