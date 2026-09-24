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

The tensor-level EDSL. A `Tensor` is a bare index into its `Graph`, with a
`Shape` (outermost first, `Shape::unknown` where an input enumerates more than
one size) and a `DType` of `float16`, `float32` or `int32`; it carries no
operators of its own, and every op is a `Graph` member: `input`, `output`,
`constant` (raw bytes, to the blob), `halfConstant` (fp32 values narrowed to
an fp16 blob tensor), `scalar` (inline), `linear`, `matmul`, `transpose`,
`reshape`, `softmax(axis)`, `sum(axis)`, `max(axis)`, `layerNorm`, `conv`,
`gather`, `concat`, `slice`, `scaledDotProductAttention`, `gelu`, `cast`, and
`apply`. Misuse is recorded rather than asserted or thrown: the op returns an
invalid `Tensor`, ops given one return another without a second error,
`isValid()` and `errors()` say what went wrong first, and `build()` of an
invalid graph is an empty `Package`.

The value layer is shared rather than duplicated. `eacp-gpu-codegen` already
builds without `eacp-graphics`, so nothing moves: a graph holds one
`GPU::ShaderBuilder`, and `apply` takes one to three tensors (or a `Vector` of
them) and a body over `GPU::Float` values, so the operators of `ShaderValue.h`
record the elementwise expression into the builder and the expression becomes
one "apply per element" op. What lowers is `Input` (an operand tensor),
`Constant` (a broadcast scalar), `Binary` (`add`, `sub`, `mul`, `real_div`),
unary minus, `Call` (`exp`, `log`, `tanh`, `sqrt`, `rsqrt`, `abs`, `floor`,
`erf`, `maximum`, `minimum`, `pow`, `clip`), `Compare` and `Select`; any other
kind, any statement, any non-scalar type and any value from outside the apply
is refused when the apply is recorded. Exact GELU is a named op all the same,
lowering to MIL's `gelu` in `EXACT` mode: the EDSL's `erf` is a polynomial
helper written for the GPU, and the engine runs `gelu` as one op. Shapes are
fixed where `input` declares them; an input may declare a list of enumerated
shapes, and no more than that (see shapes, below).

The Whisper `Net` ops are not here. They belong to the net that has them.
What is here is what those ops lower to.

### `ML/MIL` - the emitter

A writer for Core ML's ML Program format: `Model.proto` and `MIL.proto` from
coremltools' `mlmodel/format`, encoded by a small hand-rolled protobuf writer
so the runtime pulls in no protobuf library, and the `.mlpackage` directory
around it (`Manifest.json`, `Data/com.apple.CoreML/model.mlmodel`,
`Data/com.apple.CoreML/weights/weight.bin`). Every tensor constant that reaches
an output goes into the blob in the layout the spike verified, fp16 through
`appendHalf`/`halfBytes` in `MIL/Half.h`, and only scalars go inline. The
manifest's identifiers are fixed, so a package's bytes are deterministic: a
golden test pins the proto fields and the blob layout, and the cache key is
stable. `toText()` renders a graph as readable MIL for the tests to assert on,
the part `emitMetal` plays for the shader tests, and `specification()` hands
over the structure `build()` encodes. A `Package` is the three as bytes, the
model, `weight.bin` and the manifest, with `write(dir)`, which deletes what is
at the path first and so refuses an existing path that does not end in
`.mlpackage`. It carries no hash of its own: the runner's cache key is the one
hash of a package. The opset is `CoreML7` (specification 8), or `CoreML8`
(specification 9) when the program holds `scaled_dot_product_attention`, an
iOS 18 op. Only ops that reach an output are emitted, each named
`<op>_<index>` unless it is an output; a constant that reaches none takes no
index, so an unused weight neither enters the blob nor shifts a generated
name.

An emitter regression is caught the way the GLSL ones are: every program the
tests write is compiled by Core ML inside the suite wherever `eacp-ml` exists,
linked in when the target is there as `eacp-spirv` is for GLSL.

### `ML/Model` - the runner

- Compiling, through `MLModel compileModelAtURL:`, into
  `Options::cacheDirectory`, by default `ML::defaultCacheDirectory()`,
  `FilePath::appCacheDirectory() / "CoreML"`, as `<hash>.mlmodelc`, the hash
  SHA-256 cut to 32 hex digits over a format tag (`eacp-ml-cache-2`), each
  package file's path and bytes, the weights' identity (`Options::weightsName`
  and `weightsVersion` when the caller gives a name, in which case the blob is
  not hashed at all, else the blob itself) and the OS build
  (`kern.osversion`), the way the Vulkan pipeline cache names its producer. A
  `Package` is written to `<hash>.<pid>-<n>.mlpackage` beside the target and
  deleted after the compile; Core ML's output is moved to
  `<hash>.mlmodelc.<pid>-<n>.tmp` and renamed onto the target with
  `renamex_np(RENAME_EXCL)`, so one that loses a race to another process
  deletes its own copy and loads the winner's. A miss first sweeps the
  directory of `.mlpackage`, `.tmp` and `.trash` entries under a cache key
  older than an hour, what a process that died mid-compile leaves. A hit is
  loaded where it lies and never recompiled or replaced, because the spike
  found Core ML's engine cache keyed on that compiled model on disk; the one
  exception is a hit that fails to load twice running, which is renamed
  atomically to `<hash>.mlmodelc.<pid>-<n>.trash`, removed and recompiled.
  It takes two failures because Core ML reports every load failure alike
  (below). An `.mlpackage` directory goes through the same cache, keyed by
  every regular, non-hidden file under it, read through `MemoryMappedFile`,
  and an `.mlmodelc` is loaded in place.
- `ComputeUnits` as an option: all, CPU and Neural Engine, CPU and GPU, CPU.
  Default all.
- `load`/`loadAsync` and `predict`/`predictAsync`, named after
  `Processes::run`/`runAsync`, each returning a `Result` (`ok` and an `error`
  string, as `OnlineResource::Result` has). A `Model` holds a `Pimpl` and
  cannot be copied, so there is no `Async<Model>`: the object owns its state,
  `loadAsync` returns `Async<Result>` and `predictAsync` an
  `Async<Prediction>`, a `Result` with the `Outputs` in it, because an output
  the caller did not bind is allocated by the runner and has to travel back in
  the result. The jobs run in order on one serial dispatch queue per model,
  resolve on the main thread through `Threads::callAsync`, and are abandoned
  when the model is destroyed, which may happen on any thread: the
  abandonment is handed to the main thread when it is not already there. The
  blocking forms run on the caller's thread and pump no loop, since
  WhisperEACP calls them from a worker and a console app has none; a mutex
  keeps a blocking and a queued prediction on one model from running at
  once. `inputs()` and `outputs()` read the model description back,
  enumerated shapes included, and `wasCacheHit()` and `compiledPath()` say
  what the load did.
- Inputs and outputs as `ML::MultiArray`, keyed by feature name in `ML::Inputs` and
  `ML::Outputs` (both `EA::MapVector<std::string, MultiArray>`); an array bound in
  `Outputs` is passed as that output's backing and written in place. It is
  `MultiArray` rather than `Array` because the shorter name shadowed
  `eacp::Array`, EA's fixed-size container, inside the namespace, as the MIL
  writer's `StringList` keeps clear of `eacp::Strings`. A
  `MultiArray` is a shared handle: fp16 is an IOSurface-backed `OneComponent16Half`
  pixel buffer with padded rows under an `MLMultiArray`, since that is the one
  form the Neural Engine reads and writes without a copy, and fp32 and int32
  are a plain `MLMultiArray` (`initWithShape:dataType:error:`). An fp16 array
  falls back to a plain `MLMultiArray` when the IOSurface cannot be made,
  though no width up to 16,777,216 columns failed to make one on the phase 1
  machine; the Whisper logits row, 51865 columns, gets a stride of 103744
  bytes. `create` refuses an empty, negative or unknown shape. There is no
  public `data()`: `toFloats` and `fromFloats` are the host path and
  `copyTo(GPU::Buffer&, bufferType)` and `copyFrom` the seam to the kernel
  path, and each holds the pixel buffer's base-address lock for its
  duration. `GPU::Buffer` has no public
  contents pointer, so they go through its `update()` and `read()`, whose wait
  on submitted work is the ordering the seam needs; they convert between the
  array's fp16 and the buffer's type, and copy row by row where an IOSurface's
  row padding rules out a single memcpy.
- `computePlan()`, `MLComputePlan` (macOS 14.4) read back as one entry per op
  with its MIL type, the device it landed on, the devices it could have, and
  its cost estimate, so a test can assert an encoder went to the Neural Engine
  rather than silently to the GPU, and so WhisperEACP's `DeviceInfo` can print
  it (eacp has no such app; `Apps/ML` is its print). `ML::hasNeuralEngine()`
  asks `MLAllComputeDevices()` (macOS 14, iOS 17) whether there is an engine
  to place on at all.

The deployment target stays where eacp has it, macOS 11 and iOS 14: a library
that raised it would put a linker warning into every app that links it. The
runner guards with `@available` instead, and `ML::isSupported()` (macOS 13,
iOS 16, for the engine-only compute units and output backings) and
`ML::hasComputePlan()` (14.4, 17.4) say what the running OS has; the tests
self-skip on false, and `MLState` for phase 4 wants 15. That floor is not
eacp's alone to keep: the Xcode 27 on the phase 1 machine refuses
`IPHONEOS_DEPLOYMENT_TARGET` 14.0 for the simulator, whose floor there is
15.0, so the whole project's iOS target moves the day CI's Xcode does, this
module or not. `CMake/AppleSetup.cmake` forces `CMAKE_OSX_DEPLOYMENT_TARGET`
to 14.0 for iOS, so a `-DCMAKE_OSX_DEPLOYMENT_TARGET` on the command line does
not take there; the local simulator build passed 15.0 to `xcodebuild` as a
build setting instead. iOS gets the same
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
the cache, the async forms and the buffer seam. Both write under one scratch
directory per run, `<temp>/eacp-ml-tests-<pid>`, deleted at exit.
`EACP_REQUIRE_ANE=1` makes the
engine-placement assertions fail rather than skip, as `EACP_REQUIRE_GPU=1` does
for the device suites. Whether the macOS CI lane can set it is unverified:
GitHub's arm64 macOS runners are virtual machines, so `build.yml` gains the
variable only once `MLAllComputeDevices()` has been read on one.

### `Apps/ML`

`Spike` stays as the phase 0 record, and `Projection` (`MLProjection`), one
console app on the module beside it, is the worked example: it builds the
spike's projection and softmax at run time, loads it (saying whether the
compile was a cache hit), prints the compute plan and times prediction at 448,
1024 and 1500 rows under the `--units` it is given. The directory's gate is
`EACP_HAS_COREML AND NOT IOS`. `MLSpike` calls `eacp_skip_pch`, because its
own macOS 14.4 floor clashes with the shared PCH under `EACP_CI_BUILD`, which
would have failed the macOS CI job from the phase 0 commit on.

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
  cache directory, keyed by a hash of the package's bytes, and never
  recompiles a package whose key it already holds; replacing the directory
  under a hit throws the engine cache away with it.
- **The per-prediction floor is about a tenth of a millisecond.** 448 rows on
  the engine take 0.14 ms median, 1500 rows 0.23 ms, with IOSurface input and
  an output backing. The IOSurface input saves 0.03 ms against a plain
  `MLMultiArray`; the output backing saves another 0.07 ms on the engine and
  halves the GPU's time (0.66 to 0.28 ms), so `ML::MultiArray` passes both. At a
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
  compiled model is kept. The finding is about the enumerated set as such, not
  about every member size: phase 1 found a fixed 448-row model of the same two
  ops placed on the CPU under every setting (below).
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

Status as of 2026-09-24: the half a Mac can show is done. On this machine, an
M5 Max on macOS 27.0, the whole project builds and every suite passes;
`MLGraphTests` runs 73 tests (71 in a build without `eacp-ml`, the two that
run a package through Core ML left out) and `MLTests` 32, all passing with
and without `EACP_REQUIRE_ANE=1`, and `CoreTests` gained
`Files/createAndRemoveDirectories`. The ML targets build under
`EACP_CI_BUILD=ON`, and the module builds for the iOS simulator at a 15.0
floor (see the deployment target above). The CI lanes are pending: Linux,
Windows, the iOS job at CI's own floor, and whether the macOS runner has an
engine at all; `build.yml` is unchanged.

What the module is, where it differs from the text above, is written back into
that text. `MLTests` runs nano from `Tests/ML/TestMain.cpp` inside
`eacp::Apps::run`, so the async tests have a message loop to resolve on.
Measured by `MLTests`' program suite, fp16 in and out against an fp32 scalar
reference, `[1500, 384]` unless stated, max abs error (and max rel where
recorded), with the device Core ML chose:

| program | CPU | CPU and GPU | CPU and engine, and all |
| --- | --- | --- | --- |
| linear + softmax | 1.4e-3 / 3.2e-2, CPU | 1.9e-4 / 1.7e-3, GPU | 3.1e-4 / 5.8e-3, engine |
| linear, then layer norm | 3.7e-2, CPU | 5.0e-3, GPU | 9.4e-3, engine |
| elementwise `tanh(x * 0.5 + 0.25)` | 9.5e-4, CPU | the same, CPU | the same, CPU |
| layer norm alone | 3.1e-3, CPU | the same, CPU | the same, CPU |
| attention, `[1, 128, 64]` | 4.8e-3, CPU | the same, CPU | the same, CPU |

The first row reproduces the spike's. The tests' tolerances sit at about three
times these, per compute-unit setting, except the CPU's linear then layer norm,
held at 5e-2.

Phase 1 findings:

- **Placement depends on the problem's size and its op mix.** A fixed 448-row
  `linear` and `softmax` stays on the CPU under every setting; only the
  1500-row one went to the engine. A lone `layer_norm`, a lone
  `scaled_dot_product_attention`, causal or not (with the `greater` that
  builds its mask), and an elementwise-only program stay on the CPU too; a
  layer norm behind a `linear` follows it to the GPU or the engine. So
  `computePlan()` is necessary rather than a diagnostic, and a small test
  program says nothing about where the encoder will go.
- **fp16 attention ignores a float mask.** Core ML's fp16
  `scaled_dot_product_attention` ignores an additive float `attn_mask`, `-inf`
  or `-1e4`, from 32 positions up on every compute unit, though it honours one
  in fp32 and at 4 and 16 positions; a bool mask is honoured everywhere. The
  graph emits the causal mask as a 0/1 blob constant turned bool by `greater`,
  since a bool tensor cannot go in the blob, and
  `MLGraph/CoreML/causalAttentionMasksInHalfPrecision` holds it at 64.
- **`log` and `rsqrt` need an explicit `epsilon`.** Core ML rejects a program
  without one, although coremltools documents it as optional; the apply
  lowering passes coremltools' defaults, 1e-45 and 1e-12.
- **A '.' in a MIL identifier crashes the process.** Core ML dereferences null
  in `makeProgramWithMemoryLayout` instead of returning an error, so one
  malformed package takes a whole test process down. `input` and `output`
  refuse a name that is not an identifier, since the runner addresses features
  by it, and a constant's name is made one (`blocks.0.attn_ln.weight` becomes
  `blocks_0_attn_ln_weight`).
- **A damaged compiled model cannot be told from any other load failure.**
  Core ML reports every load failure as domain `com.apple.CoreML`, code 0. A
  damaged `coremldata.bin` shows only as underlying code 3, "not a valid
  .mlmodelc", and a truncated `weight.bin` as "Failed to build the model
  execution plan ... -14"; a damaged `model.mil` crashes the process inside
  `makeProgramWithMemoryLayout`, the same crash a '.' in a name causes, and
  nothing in the runner can guard against that one. Hence the cache retries a
  failed hit once before it discards and recompiles it.
- **An interrupted compile leaves its directories behind.** A process that
  dies between writing `<hash>.<pid>-<n>.mlpackage` or staging `.tmp` and the
  rename leaves them in the cache directory, so a miss sweeps any such entry,
  and any `.trash`, older than an hour.

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
  the set on the Core ML path. Phase 1 put a 448-row projection on the CPU, so
  the engine is not assumed for the small members: phase 3 reads the plan per
  member, and a member the engine refuses is a finding for the benchmark, not
  a silent CPU run.
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
  other Async in eacp. The blocking `predict()` runs on the caller's thread,
  and a mutex serialises it against the queue, so one model runs one
  prediction at a time whichever form asked. A `Model` may be destroyed on any
  thread; what it handed out is abandoned on the main thread. The live
  transcriber's encoder call becomes an Async rather than a commit-and-wait.
- **Placement is asserted, not assumed.** A test that wants the engine reads
  the plan. A build that cannot get it fails under `EACP_REQUIRE_ANE=1` and
  skips otherwise. Phase 1 showed why it cannot be assumed: Core ML places by
  size and op mix, and a lone layer norm, attention or elementwise program
  stays on the CPU however the compute units are set.
- **Causal attention takes a bool mask.** Core ML's fp16 attention ignores a
  float additive mask from 32 positions up, so the Graph's causal
  `scaledDotProductAttention` builds a bool one, and the Core ML `Net`'s
  decoder, if phase 4 comes, does the same.

## Risks

- The ML Program spec is read out of coremltools' protos rather than a
  document. The spike settled the blob and the ops it used byte for byte; each
  op the encoder adds is a fresh chance of a field Core ML reads otherwise,
  which is why every package the tests write is compiled in the suite. Phase 1
  found two such fields (the required `epsilon`, the ignored float mask) and
  two ways to crash Core ML outright (a '.' in a name, a damaged `model.mil`
  in a compiled model), so a malformed package can end a test process rather
  than fail a test.
- Enumerated shapes are the behaviour Apple documents least. The spike kept a
  small set on the engine; eighteen members cost more at a cold load, and if
  that grows past what a first run tolerates, the fallback, one model per
  context compiled on first use, is a cache-size cost and nothing else.
- The per-prediction floor is fine for a 30 s window, and with the host round
  trip around it, it is what rules the decoder out until measured.
- The macOS CI lane may have no Neural Engine to place on. If it has none,
  placement is asserted only on a developer's Mac, and a change that moves the
  encoder off the engine is caught by the benchmark rather than by CI. Still
  open after phase 1: `ML::hasNeuralEngine()` is the question to ask on the
  runner, and `build.yml` does not set `EACP_REQUIRE_ANE` until it has been
  asked.
- Core AI may become the only way to reach new engine features. The seam is
  the insurance: nothing above `Net` knows which Apple framework is under it.

## Gaps for eacp, as they surface

The section WhisperEACP's own plan keeps, kept here for the same reason: a gap
belongs in eacp, not in a workaround downstream. Phase 1 surfaced these inside
eacp itself:

- `eacp-core` had no way to create or remove a directory tree, so the
  package writer and the cache went through `toStdPath` and
  `std::filesystem`. Filled in this phase: `Files::createDirectories` and
  `Files::removeAll` (`Core/Utils/Files.h`), which `OnlineResource` now uses
  too; only the cache's directory walks still use `std::filesystem`.
- EA's `Span` refuses a temporary `Vector`, so
  `fromFloats(Vector<float> {...})` does not compile and the caller names the
  vector first. Still open.
- The iOS deployment target of 14.0 is refused by Xcode 27's simulator, whose
  floor is 15.0, and `CMake/AppleSetup.cmake` forces 14.0 over a command-line
  override; the whole project moves when CI's Xcode does. Still open.
- Nothing swept the `<hash>.<pid>-<n>.mlpackage` and `.tmp` directories a
  process that dies mid-compile leaves in the Core ML cache. Closed in this
  phase: a miss sweeps them, and `.trash`, once older than an hour.
