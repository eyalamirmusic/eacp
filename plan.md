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
behind, written once against this seam:

```cpp
enum class Activation { none, gelu };

struct Net
{
    virtual Tensor input(const Binding& source, Shape shape, DType type) = 0;
    virtual void output(Tensor value, const Binding& target) = 0;
    virtual Tensor rows(const Weight& table, int first, int count) = 0;
    virtual Tensor transpose(Tensor matrix) = 0;

    virtual Tensor conv1d(Tensor frames, const Weight& weight, const Weight& bias,
                          int stride, int padding, Activation activation) = 0;
    virtual Tensor linear(Tensor input, const Weight& weight, const Weight* bias,
                          Activation activation = Activation::none) = 0;
    virtual Tensor linearAdd(Tensor input, const Weight& weight,
                             const Weight* bias, Tensor stream) = 0;
    virtual Tensor add(Tensor a, Tensor b) = 0;
    virtual Tensor layerNorm(Tensor input, const Weight& weight,
                             const Weight& bias) = 0;
    virtual Tensor attention(Tensor queries, Tensor keys, Tensor values,
                             int heads, bool causal) = 0;

    virtual Tensor embed(Tensor tokens, const Weight& tokenTable,
                         const Weight& positionTable, int firstPosition) = 0;
    virtual Tensor appendLinear(Cache& cache, Tensor input, const Weight& weight,
                                const Weight* bias) = 0;
    virtual void argmax(Tensor logits, int row, Tensor mask, Tensor slot) = 0;
};
```

Each op answers a call the code makes today. `input` and `output` are the
outside buffers: the mel, the encoder rows, the logits, the token slots and the
masks, bound ranges on the kernel backend and the model's features on Core ML.
`rows` is the positional prefix the encoder adds, a view with no dispatch.
`transpose` is the band-major mel turned into the frames conv1 reads, which on
the kernel backend is a stride swap `Unfold` already takes. `conv1d` carries
its padding. A bias is a pointer because `k_proj` and the tied logits have
none. `linearAdd` is the residual, and it names the stream it adds into,
because the kernel path's residual is a store into `hidden` in place, not a sum
of two tensors; on Core ML it is a `linear` and an `add`. `embed` gathers the
token and the position table in one store from a first position, as
`Kernels/Embed.h` does. `appendLinear` is a K/V projection written straight
into the cache rows the step owns, today's `BufferRange` bind, since a separate
append would cost a copy dispatch per projection per layer per step; the cross
K/V go through it after a reset in `beginSequence`. `argmax` takes the logits
row it reads (the prompt's last) and the token slot the next `embed` reads on
the device, and it is called from `Whisper`, not from the decoder body. There
is no `softmax`: the encoder folds it into the apply and the decoder runs it
only inside attention, so nothing at this level calls one.

The ops sit at that level, above matmul and softmax, on purpose. The kernel
backend fuses the softmax into the apply and the GELU into the store; the Core
ML backend lowers `attention` to the fused `scaled_dot_product_attention` op
the Neural Engine handles as one. Ops at the matmul level would take that
freedom from both. The list is counted rather than guessed: the encoder is 46
dispatches, a decode step 61 and the prompt step 81, and the encoder's 42
compute calls against this seam come out on the kernel backend as those 46 in
today's order (a `conv1d` is two, `rows` and `transpose` none, the rest one
each). Phase 2 is still where it is confirmed, since writing both bodies
against it is what shows nothing was missed.

Two backends:

- **The kernel backend** is today's code moved one file over. A `Tensor` is a
  `GPU::Buffer` plus a shape, `linear` picks the tiled or split program by row
  count and weight storage, and barriers go in by read-after-write and
  write-after-read on buffers, which puts back exactly the ones there are
  today, the barrier-free q/k/v trio included. The three GPU platforms keep
  working unchanged and the tests keep their bit-exact references.
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
  in flight. Every Core ML prediction is a host round trip, the prediction's
  own floor plus the call and the copies, so for tiny.en the engine loses that
  race. A KV cache on Core ML is possible, with `MLState` (macOS 15) and a
  fixed cache length behind a mask, which is how WhisperKit does it. It is
  phase 4, and only if phase 3's numbers say so.
- The seam between the two halves is one buffer of encoder rows, which the
  decoder already takes as a `Buffer`. The Core ML encoder's output is copied
  into one: a copy into shared storage on Metal, widened from fp16 on the way,
  2.3 MB per window.

The mel front end stays on the GPU too. It is kernels, it is cheap, and its
output crosses the same seam the other way: the first 2n frames of each band,
after the mel pass has been committed and waited on, where today it shares the
encoder's pass.

The payoff is specific to the live transcriber: the encoder runs on the Neural
Engine while the GPU decodes the previous window and draws the UI. Today the
three contend for one device.

## What lands in eacp

A new module, `Lib/eacp/ML`, split the way `GPU` is. `eacp-ml-graph` is the
graph builder and the MIL, protobuf and blob writers: bytes in and bytes out,
linking `eacp-core` and `eacp-gpu-codegen` and nothing with a device, so it
builds everywhere as `eacp-gpu-codegen` does and its tests run on every CI
lane. `eacp-ml` is the Core ML runner, Apple only, behind a seventh capability
variable `EACP_HAS_COREML` beside the six in the top-level `CMakeLists.txt`, on
where `EACP_HAS_GPU` is and the platform is Apple, and a PUBLIC define on
`eacp-ml` the way `EACP_HAS_CONTEXT` is one on `eacp-graphics`. Everything Core
ML stays inside the runner: no header an app includes names an `MLModel`.

### `ML/Graph` - the program builder

The tensor-level EDSL. `Tensor` handles with the value operators the shader
EDSL already has (`+`, `*`, `exp`, `select`, comparisons, uniforms as scalar
inputs), plus the axis-level ops the shader EDSL cannot spell: `matmul`,
`transpose`, `reshape`, `softmax(axis)`, `sum(axis)`, `max(axis)`, `layerNorm`,
`conv`, `gather`, `concat`, `slice`, `scaledDotProductAttention`, and `gelu`.

The value layer is shared rather than duplicated. `eacp-gpu-codegen` already
builds without `eacp-graphics`, so nothing moves: a graph holds one
`GPU::ShaderBuilder`, the operators of `ShaderValue.h` record an elementwise
expression on tensors into it, and the expression becomes one "apply per
element" op. What lowers is `Input` (an operand tensor), `Constant` (a
broadcast scalar), `Binary` (`add`, `sub`, `mul`, `real_div`), unary minus,
`Call` (`exp`, `log`, `tanh`, `sqrt`, `rsqrt`, `abs`, `floor`, `erf`, `maximum`,
`minimum`, `pow`, `clip`), `Compare` and `Select`; any other kind, any
statement and any non-scalar type is refused when the apply is recorded. Exact
GELU is a named op all the same, lowering to MIL's `gelu`: the EDSL's `erf` is a
polynomial helper written for the GPU, and the engine runs `gelu` as one op.
Shapes are fixed at `prepare()`; a graph may declare a list of enumerated
shapes for an input, and no more than that (see shapes, below).

The Whisper `Net` ops are not here. They belong to the net that has them.
What is here is what those ops lower to.

### `ML/MIL` - the emitter

A writer for Core ML's ML Program format: `Model.proto` and `MIL.proto` from
coremltools' `mlmodel/format`, encoded by a small hand-rolled protobuf writer
so the runtime pulls in no protobuf library, and the `.mlpackage` directory
around it (`Manifest.json`, `Data/com.apple.CoreML/model.mlmodel`,
`Data/com.apple.CoreML/weights/weight.bin`). Every tensor constant goes into
the blob in the layout the spike verified and only scalars go inline. The
manifest's identifiers are fixed, so a package's bytes are deterministic: a
golden test pins the proto fields and the blob layout, and the cache key is
stable. `toText()` renders a graph as readable MIL for the tests to assert on,
the part `emitMetal` plays for the shader tests.

An emitter regression is caught the way the GLSL ones are: every program the
tests write is compiled by Core ML inside the suite wherever `eacp-ml` exists,
linked in when the target is there as `eacp-spirv` is for GLSL.

### `ML/Model` - the runner

- Compiling, through `MLModel compileModelAtURL:`, into
  `FilePath::appCacheDirectory() / "CoreML" / "<hash>.mlmodelc"`, the hash
  taken over the program bytes, the weights' identity (a name and version the
  caller gives, or else a hash of the blob) and the OS build, the way the
  Vulkan pipeline cache names its producer. The compile lands in a temporary
  directory and is installed by an atomic rename; one that loses a race to
  another process deletes its own copy. A hit is loaded where it lies and never
  recompiled or replaced, because the spike found Core ML's engine cache keyed
  on that compiled model on disk. An empty cache directory compiles every time.
- `ComputeUnits` as an option: all, CPU and Neural Engine, CPU and GPU, CPU.
  Default all.
- `load`/`loadAsync` and `predict`/`predictAsync`, named after
  `Processes::run`/`runAsync`. A `Model` holds a `Pimpl` and cannot be copied,
  so there is no `Async<Model>`: the object owns its state and the async forms
  return `Async<Result>` or `Async<void>`, in the job shape `OnlineResource`
  has, a job shared with a per-model serial dispatch queue, resolved on the
  main thread, abandoned when the model is destroyed. The blocking forms run on
  the caller's thread and pump no loop, since WhisperEACP calls them from a
  worker and a console app has none.
- Inputs and outputs as `ML::Array`: an `MLMultiArray` over an IOSurface-backed
  pixel buffer in `OneComponent16Half` where the tensor is fp16, since that is
  the one form the Neural Engine reads and writes without a copy, and a plain
  `MLMultiArray` otherwise. `Array::copyTo(GPU::Buffer&)` and `copyFrom` are
  the seam to the kernel path. `GPU::Buffer` has no public contents pointer,
  so they go through its `update()` and `read()`, whose wait on submitted work
  is the ordering the seam needs; they convert between the array's fp16 and
  the kernel path's fp32, and copy row by row where an IOSurface's row padding
  rules out a single memcpy.
- `computePlan()`, `MLComputePlan` (macOS 14.4) read back as one entry per op
  with the device it landed on and its cost estimate, so a test can assert an
  encoder went to the Neural Engine rather than silently to the GPU, and so
  WhisperEACP's `DeviceInfo` can print it (eacp has no such app; `Apps/ML` is
  its print).

The deployment target stays where eacp has it, macOS 11 and iOS 14: a library
that raised it would put a linker warning into every app that links it. The
runner guards with `@available` instead, and `ML::isSupported()` (macOS 13,
iOS 16, for the engine-only compute units and output backings) and
`ML::hasComputePlan()` (14.4, 17.4) say what the running OS has; the tests
self-skip on false, and `MLState` for phase 4 wants 15. iOS gets the same
module; the simulator runs CPU only and the tests self-skip there as the GPU
ones do. Library `.mm` files are compiled without ARC, as every eacp target
is, so the spike's ARC code is ported onto `ObjC::Ptr` and `AutoReleasePool`
rather than copied.

### `Tests/ML`

Two suites in one directory, as `Tests/GPU` has. `MLGraphTests` links
`eacp-ml-graph` alone and runs everywhere: shape inference and refused ops,
the apply lowering, protobuf and blob bytes against golden values, the MIL
text per op and the package layout, with every package compiled by Core ML
where `eacp-ml` exists. `MLTests` is the device half: a handful of small
programs (elementwise chain, matmul, softmax over an axis, layer norm, one
attention block) run on the CPU and the Neural Engine and checked against an
fp32 scalar reference at a tolerance measured per compute-unit setting, and
the cache, the async forms and the buffer seam. `EACP_REQUIRE_ANE=1` makes the
engine-placement assertions fail rather than skip, as `EACP_REQUIRE_GPU=1` does
for the device suites. Whether the macOS CI lane can set it is unverified:
GitHub's arm64 macOS runners are virtual machines, so `build.yml` gains the
variable only once `MLAllComputeDevices()` has been read on one.

### `Apps/ML`

`Spike` stays as the phase 0 record, and one console app on the module beside
it is the worked example: it builds the spike's projection and softmax at run
time, writes and loads it (saying whether the compile was a cache hit), prints
the compute plan and times prediction at a few sizes. The directory's gate
becomes `EACP_HAS_COREML AND NOT IOS`.

## What lands in WhisperEACP

On a branch of the same name, `EDSL-CoreML`, configured against this tree with
`-DCPM_eacp_SOURCE=$HOME/Code/eacp` until the eacp side is pushed and
`develop` carries it. Its `CLAUDE.md` says that override is for exactly this
case: an eacp change made alongside the WhisperEACP change that needs it.

- `Lib/WhisperEACP/Net/`: the `Net` seam, the kernel backend extracted from
  `Encoder.cpp` and `Decoder.cpp`, and the Core ML backend over `eacp::ML`.
  `Encoder::encode` and the decoder's two recording calls become the shared
  bodies, behind the same classes. `Whisper::prepare` takes a backend choice:
  kernels, or Core ML for the encoder.
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
| time to write and compile the package, first and second time | write 4.2 / 4.4 ms (Debug, 296 KB blob); compile 20 / 14 ms; first engine load 95 ms, then 5 ms |
| prediction latency at that shape, CPU-and-engine versus all | median 0.33 / 0.34 ms plain, 0.30 / 0.30 IOSurface in, 0.23 / 0.24 IOSurface in and out |
| did the two ops land on the Neural Engine | as `linear` + `softmax`, both, under both settings; as `matmul` + `softmax`, neither: CPU under every setting |
| max abs and rel error against an fp32 CPU reference | engine 3.3e-4 / 5.7e-3, GPU 1.7e-4 / 2.0e-3, CPU 1.5e-3 / 3.2e-2 (rel where the reference is at least 1e-4) |
| blob weights against inline constants: does either matter | not to prediction; inline costs 24 against 4 ms to write, 29 against 20 ms to compile and 30 ms on every load |

Measured on an M4 Max, macOS 26.6, by `Apps/ML/Spike` (`MLSpike` with no
arguments writes every variant through `MILWriter.h` and measures it;
`--op matmul` keeps the CPU-placement finding reproducible). The weights are a
seeded fp16 `[384, 384]`, the input a seeded fp16 `[1500, 384]`, 20 timed runs
after 3 warm ones.

Spike findings:

- **`linear`, not `matmul`.** A `matmul` against a constant weight is placed on
  the CPU by the compute plan under every compute-unit setting, at rank 2 and
  rank 3, and drags the `softmax` after it there too, although the plan lists
  the engine as supported for both. The same computation as a `linear` op
  (weight `[out, in]`, bias always present, zeros when the net has none, which
  is what coremltools emits) puts both ops on the Neural Engine. So the Graph
  lowers a projection against a weight to `linear`, never to `matmul`, and the
  `Net::linear` op maps onto it directly. `[out, in]` is the order the
  safetensors already store, so the weight goes into the blob untransposed.
  `matmul` stays for two activations, the attention scores and apply.
- **The engine cache is keyed on the compiled model on disk.** A first load
  onto the engine costs 75-95 ms at this size and the plan read as much again;
  a second load of the same `.mlmodelc`, in the same process or a new one,
  costs 5-9 ms. A recompile or a copy pays the full cost again, even a
  recompile of a byte-identical package moved onto the same path. For
  `ML/Model` that means the cache compiles once to a stable path under the
  cache directory, keyed by the package's content hash, and never recompiles a
  package whose hash it already holds; replacing the directory under a hit
  throws the engine cache away with it.
- **The per-prediction floor is about a tenth of a millisecond.** 448 rows on
  the engine take 0.14 ms median, 1500 rows 0.23 ms, with IOSurface input and
  an output backing. The IOSurface input saves 0.03 ms against a plain
  `MLMultiArray`; the output backing saves another 0.07 ms on the engine and
  halves the GPU's time (0.66 to 0.28 ms), so `ML::Array` passes both. At a
  single projection this small the CPU is as fast as the engine (0.16 ms): the
  engine's case is the whole encoder, and the decoder's is still phase 4's to
  measure.
- **fp16 accuracy differs by device.** The engine and the GPU agree with the
  fp32 reference to a few units in the last fp16 place (max abs 3.3e-4 and
  1.7e-4); the CPU's fp16 path is the least accurate, max abs 1.5e-3 and max
  relative 3.2e-2. A tolerance measured on the engine does not transfer to a
  CPU-only run, so the tests measure it per compute-unit setting.
- **Enumerated shapes stay on the engine.** An input enumerated over 448, 512,
  1024 and 1500 rows (1500 the default) keeps `linear` and `softmax` on the
  Neural Engine, runs at 448 and 1500 rows with the fixed model's latency and
  error, and costs one engine program per shape at load: 238 ms cold against
  95 ms for the fixed shape, 6 ms warm. The shapes decision below stands; the
  cold-load cost grows with the size of the set, which is one more reason the
  compiled model is kept.
- **Weights go in the blob.** Inline constants changed neither placement nor
  latency, and cost at every step that is not a prediction: write 24 against
  4 ms, compile 29 against 20 ms, and every load, cold or warm, 30 ms more,
  since the program is parsed with the weights in it. Every tensor constant
  goes to `weight.bin`; only scalars (axes, flags) go inline, as coremltools
  does. The blob layout Core ML accepts is `MILBlob` storage version 2, a
  64-byte header, each entry a 64-byte metadata record on a 64-byte boundary
  with the data right after it, and the program's `BlobFileValue.offset`
  pointing at the record, not the data.

The spike also settles the two format questions that documentation does not:
the exact blob layout Core ML accepts, and whether an enumerated-shape input
keeps a matmul on the engine.

### Phase 1 - the eacp module

`eacp-ml-graph` with the op set the Whisper encoder needs and no more,
`eacp-ml`, `Tests/ML`, `Apps/ML`, the capability variable, and the docs it
owes: a module row and a capability row in README.md's tables, "six" capability
variables becoming seven there and in CLAUDE.md, and README's "Two pieces of
the gated modules are portable" (CLAUDE.md's "two device-free pieces") becoming
three. Done when `MLGraphTests` passes on every CI lane, `MLTests` passes with
`EACP_REQUIRE_ANE=1` on a Mac with an engine and self-skips cleanly on one
without, and the module builds for iOS.

### Phase 2 - the seam, with no behaviour change

In WhisperEACP: `Net`, the kernel backend, the encoder and decoder rewritten
against it. Done when every existing test passes unchanged, the encoder and
decoder outputs are bit identical to before (the oracle tests are the check),
and the benchmark's `WhisperEACP` column is within noise of the number before
the refactor. No test today holds a pre-refactor output, so bit identity is
shown one of two ways: a one-off dump of the encoder rows and the logits taken
before the refactor and compared after it, or dispatches unchanged by
construction, the encoder's compute calls mapping one to one onto today's
dispatches with today's uniforms in today's order. `Encoder` and `Decoder`
stay as public facades that own a kernel `Net`, because the tests and the
decoder oracle drive `Encoder::encode`, `beginSequence` and `step` directly
and build their weights from `SafeTensors`. The op list above is where this
phase starts, and writing both bodies against it is what confirms it.

### Phase 3 - the Core ML encoder

The Core ML `Net` backend, the encoder built through it at prepare, the
enumerated audio-context shapes, the backend's own layout transposes, the seam
copies, `Tests/Net`, the oracle contestant, the benchmark column, the live
transcriber running the encoder on the engine. Done when the transcript
matches on the test audio, the row-by-row tolerance is measured and written
down, and the benchmark shows the encoder's wall time on the engine beside its
Metal time, with the GPU idle during it.

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
  over fewer than 1500 positions (`Whisper::audioContextForSamples`, multiples
  of 64 from a floor of 448, clamped to 1500), which is eighteen contexts: 448
  to 1472 in steps of 64, and 1500. The program declares those as enumerated
  shapes, one compiled program per member. A context outside the set cannot be
  served by a larger member and trimmed, because attention sees every position
  it is given: 640 positions cut to 576 is a different answer from 576, and
  the oracle already asserts that the 576 transcript differs from the
  window's. A context rounded up to a member is therefore the effective
  context for the encoder and the decoder alike, since `beginSequence` takes
  the same count. `audioContextForSamples` only yields members;
  `setAudioContext`, which takes any count up to 1500, is validated against
  the set on the Core ML path.
- **Layout.** The encoder is frame-major throughout: conv1's store does the
  permute, and everything after it, the decoder included, reads
  `[positions, width]` rows. The only transpose the net asks for is of the
  band-major mel at its input. The engine wants channels on the second axis,
  and getting into and out of that is the Core ML backend's own business,
  below the seam: its program ends by turning back to frame-major rows, so the
  copy across the seam is a straight copy.
- **Weights.** Every tiny.en tensor ships as F32 and every value in them is
  exactly representable in fp16, which WhisperEACP's
  `Model/TinyEn/weightsAreExactlyHalves` asserts over the whole file, so the
  blob is fp16 with no weight error at half the size. `nn.Linear`'s
  `[out, in]` and the convolutions' `[out, in, k]` are MIL's `linear` and
  `conv` weight layouts as they stand, so each tensor goes from the safetensors
  bytes into the blob with nothing reordered. The kernel path keeps its own
  upload, and the Core ML path skips the GPU upload of the encoder's weights.
  A model is therefore built and compiled once per machine per model version,
  and the cache key says so.
- **Threads.** Core ML predictions run on their own queue; `predictAsync()`
  resolves on the message thread through `Threads::callAsync` like every
  other Async in eacp. The live transcriber's encoder call becomes an Async
  rather than a commit-and-wait.
- **Placement is asserted, not assumed.** A test that wants the engine reads
  the plan. A build that cannot get it fails under `EACP_REQUIRE_ANE=1` and
  skips otherwise.

## Risks

- The ML Program spec is read out of coremltools' protos rather than a
  document. The spike settled the blob and the ops it used byte for byte; each
  op the encoder adds is a fresh chance of a field Core ML reads otherwise,
  which is why every package the tests write is compiled in the suite.
- Enumerated shapes are the behaviour Apple documents least. The spike kept a
  small set on the engine; eighteen members cost more at a cold load, and if
  that grows past what a first run tolerates, the fallback, one model per
  context compiled on first use, is a cache-size cost and nothing else.
- The per-prediction floor is fine for a 30 s window, and with the host round
  trip around it, it is what rules the decoder out until measured.
- The macOS CI lane may have no Neural Engine to place on. If it has none,
  placement is asserted only on a developer's Mac, and a change that moves the
  encoder off the engine is caught by the benchmark rather than by CI.
- Core AI may become the only way to reach new engine features. The seam is
  the insurance: nothing above `Net` knows which Apple framework is under it.

## Gaps for eacp, as they surface

The section WhisperEACP's own plan keeps, kept here for the same reason: a gap
belongs in eacp, not in a workaround downstream. Empty until phase 2.
