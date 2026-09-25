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
enum class DType { float32, float16, int32 };

using Weight = TensorBuffer; // the safetensors tensor, its shape included

class Shape; // up to rank 4, outermost first

struct Binding { GPU::BufferRange range; Shape capacity; };
struct Cache { int id = -1; int rows = 0; void clear(); };

class Tensor; // a ref-counted handle into one recording

class Net
{
public:
    virtual Tensor input(const Binding& source, const Shape& shape, DType type) = 0;
    virtual void output(const Tensor& value, const Binding& target) = 0;

    virtual Tensor rows(const Weight& table, int first, int count) = 0;
    virtual Tensor transpose(const Tensor& matrix) = 0;
    virtual Tensor cached(const Cache& cache) = 0;

    virtual Tensor conv1d(const Tensor& frames, const Weight& weight,
                          const Weight& bias, int stride, int padding,
                          Activation activation) = 0;
    virtual Tensor linear(const Tensor& input, const Weight& weight,
                          const Weight* bias, Activation activation) = 0;
    virtual Tensor linearAdd(const Tensor& input, const Weight& weight,
                             const Weight* bias, const Tensor& stream) = 0;
    virtual Tensor add(const Tensor& stream, const Tensor& addend) = 0;
    virtual Tensor layerNorm(const Tensor& input, const Weight& weight,
                             const Weight& bias) = 0;
    virtual Tensor attention(const Tensor& queries, const Tensor& keys,
                             const Tensor& values, int heads, bool causal) = 0;

    virtual Tensor embed(const Tensor& tokens, const Weight& tokenTable,
                         const Weight& positionTable, int firstPosition) = 0;
    virtual Tensor appendLinear(Cache& cache, const Tensor& input,
                                const Weight& weight, const Weight* bias) = 0;
    virtual Cache makeCache(int capacityRows, int width) = 0;
};
```

Each op answers a call the code makes today. `input` and `output` are the
outside buffers: the mel, the encoder rows, the logits, the token slots and the
masks, bound ranges on the kernel backend and the model's features on Core ML;
a `Binding` carries the capacity of what it points at, which can be more than
the shape a run reads. `rows` is the positional prefix the encoder adds, and
`transpose` the band-major mel turned into the frames conv1 reads; on the
kernel backend both are views with no dispatch, the first a byte offset into
the table and the second a stride swap `Unfold` already takes. `conv1d` carries
its padding. A bias is a pointer because `k_proj` and the tied logits have
none. `linearAdd` is the residual, and it names the stream it adds into,
because the kernel path's residual is a store into `hidden` in place, not a sum
of two tensors; on Core ML it is a `linear` and an `add`. `add` is the same
shape: it consumes its stream and returns the sum, which on the kernel backend
is the stream's own buffer. `embed` gathers the token and the position table in
one store from a first position, as `Kernels/Embed.h` does. `appendLinear` is a
K/V projection written straight into the rows the cache holds next, today's
`BufferRange` bind, since a separate append would cost a copy dispatch per
projection per layer per step, and it returns the cache with them in it; the
cross K/V go through it after a reset in `beginSequence`. `makeCache` sizes a
cache once and `cached` reads one back as a tensor. The argmax is not a `Net`
op: it takes the logits row it reads (the prompt's last) and the token slot the
next `embed` reads on the device, it is dispatched from `Whisper`, not from the
decoder body, and it only joins the net in phase 4, where the model would hold
it. There is no `softmax`: the encoder folds it into the apply and the decoder
runs it only inside attention, so nothing at this level calls one.

The ops sit at that level, above matmul and softmax, on purpose. The kernel
backend fuses the softmax into the apply and the GELU into the store; the Core
ML backend lowers `attention` to the fused `scaled_dot_product_attention` op
the Neural Engine handles as one. Ops at the matmul level would take that
freedom from both. The list is counted rather than guessed: the encoder is 46
dispatches, a decode step 61 and the prompt step 81, and the encoder's 42
compute calls against this seam come out on the kernel backend as those 46 in
today's order: the two `conv1d` four, `rows` and `transpose` none, the `add`
one, the nine calls of each of the four layers 40, since `attention` is two
there (the scores, then the apply with the softmax folded in), and the final
`layerNorm` one. Phase 2 confirmed it (below).

Two backends:

- **The kernel backend** (`KernelNet`) is today's code moved one file over. A
  `Tensor` is a ref-counted handle whose liveness places it: while a copy of
  it is alive it holds a scratch buffer reserved at prepare, and the buffer is
  free again when the last copy goes. `linear` picks the tiled or split
  program by row count and weight storage, and barriers are derived from
  per-buffer read and write tracking, which puts back exactly the ones there
  are today, the barrier-free q/k/v trio included. The three GPU platforms keep
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
an fp16 blob tensor), `scalar` (inline), `linear` (with a bias, or without
one, when a zero bias of x's type goes into the blob with it), `matmul`,
`transpose`, `reshape`, `softmax(axis)`, `sum(axis)`, `max(axis)`,
`layerNorm`, `conv`, `gather`, `concat`, `slice`, `sliceLike` (x from its
start to another tensor's extents: a plain slice where the reference is
fixed, MIL's `shape` feeding `slice_by_index`'s `end` where it enumerates, so
the positional table is cut to the rows a context has at run time),
`scaledDotProductAttention`, `gelu`, `cast`, and `apply`. Misuse is recorded rather than asserted or thrown: the op returns an
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
  the result, together with how long the job waited on the queue
  (`queueWaitSeconds`) and how long Core ML's prediction call ran
  (`predictSeconds`), both measured on the queue so neither includes the hop
  back to the main thread. The jobs run in order on one serial dispatch
  queue per model, resolve on the main thread through `Threads::callAsync`, and are abandoned
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
  row padding rules out a single memcpy. Each also takes a byte offset into
  the buffer and a row stride in bytes, the buffer's rows longer than the
  array's and the bytes between them left alone, which is the mel binding:
  `[80, 3000]` band-major, of which a context of N reads the first 2N frames
  of each band. The packed forms are those at offset 0 with a stride of one
  row, and a stride shorter than a row or a last row past the buffer's end
  copies nothing. They take the buffer and an offset rather than a
  `GPU::BufferRange`, whose `const Buffer*` a write cannot go through.
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
self-skip on false, and `MLState` for phase 4 wants 15.
`ML::supportsSpecification(version)` says whether the OS loads a program of
the specification version `specification()` reports: 8, the `CoreML7`
opset, wants macOS 14 and iOS 17, and 9, which `scaledDotProductAttention`
forces, macOS 15 and iOS 18, so a caller on an older OS builds the
attention from `matmul` and `softmax` instead; a version above 9 is false. That floor is not
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
for the device suites. The macOS CI lane cannot set it: GitHub's arm64 macOS
runners are virtual machines, and phase 1's CI run found the `macos-26-arm64`
runner placing every op on the CPU under every setting, so it has no engine
and `build.yml` must not set the variable.

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

- `Lib/WhisperEACP/Net/`: the `Net` seam, the kernel backend (`KernelNet`)
  extracted from `Encoder.cpp` and `Decoder.cpp`, and the Core ML backend over
  `eacp::ML`. `Encoder::encode` and the decoder's two recording calls become
  the shared bodies, behind the same classes. The backend choice, kernels or
  Core ML for the encoder, arrives with phase 3, when there is a second backend
  to choose, and it is `Whisper::setEncoderBackend` before `prepare` rather
  than a `prepare` parameter: the `setPacksWeights` pattern, a setting read
  where the weights are built, with a call after `prepare` a `logic_error` and
  a backend the machine cannot run a `ModelError` out of `prepare` itself.
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
floor (see the deployment target above). A branch push does not trigger
`build.yml`, which runs on pushes to main and develop, on pull requests and on
`workflow_dispatch`, so the one CI run was a manual dispatch. Linux ran 71
`MLGraphTests` green on all three lanes. Windows ran 71 with one failure, the
package-layout test reading `model.mlmodel` back through the text-mode
`Files::readFile`, which folds CR LF there; it now reads the bytes back through
`MemoryMappedFile`. The iOS job built the module at CI's 14.0 floor with no
warning, but builds no ML tests or apps, since `Tests` and `Apps` gate them on
`NOT IOS`. macOS ran 73 and 32 with two tolerance failures, both programs held
to the bound of the requested setting while every op ran on the CPU; the bound
now follows the compute plan, the CPU's wherever every op landed there or no
plan is available, except that under `EACP_REQUIRE_ANE=1` the engine settings
keep the engine's. That run also answered the question left open: the
`macos-26-arm64` runner places every op on the CPU under every setting, so it
has no engine, `build.yml` must not set `EACP_REQUIRE_ANE`, and engine placement
is asserted only on a developer's Mac; `build.yml` is unchanged.

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

Status as of 2026-09-24: done on a Mac, committed on WhisperEACP's
`EDSL-CoreML` as `ec0b7c0`, branched from `FP16Work` and configured against this tree with
`-DCPM_eacp_SOURCE`. WhisperEACP pins eacp's `develop`, and this branch is
based on `main` and lacks three `develop` commits; it configured all the same,
built without a warning and passed everything. The seam is
`Lib/WhisperEACP/Net` (`whisper-net`): `Net.h` and `Net.cpp` are the interface
above, `KernelNet` the kernel backend. `Encoder::encode` records through
`recordEncoder`, and the decoder's two recording calls through
`recordSequenceStart` and `recordDecoderStep`, each written once against `Net`,
behind the same two facade classes with their public APIs unchanged.
`Whisper::prepare` takes no backend choice yet: with one backend it would be a
parameter with one value, so it comes with phase 3. The suite ran 316 of 316
before and after, the whisper.cpp oracle included.

Bit identity is shown both ways. A one-off tool, kept outside the tree because
it reaches into eacp's Metal internals, dumped the encoder rows and every
greedy step's logits for jfk.wav, with packed and with float weights, at the
full window and at 576 positions: 18 files, all `cmp`-equal before and after.
The same tool recorded every call the Metal backend makes on the compute
encoder (pipeline, buffer and offset, uniform bytes, grid, barrier) through a
stand-in `MTLComputeCommandEncoder`, and the traces before and after are
identical, so the dispatches are unchanged by construction. The trace caught
one real bug on the first run after the refactor, a `pass.dispatch` where
`AttentionApply::dispatch` pads the grid height, fixed before the final runs.
It also counts them: the encoder is 46 dispatches and 37 barriers,
`beginSequence` 8 and none, a prompt step 71 and 62, a single step 59 and 50.
With `Argmax`'s two the steps are the 81 and 61 above, which count whole
command buffers with the argmax in.

The benchmark, Release, 30 runs, took a transcribe median of 14.23 and
14.28 ms before and 14.31 and 14.39 after, so it was run again as A B B A
twice against an untouched pre-refactor tree built from `git archive`: before
14.12, 14.31, 14.39 and 14.37 ms (mean 14.30), after 14.20, 14.31, 14.20 and
14.42 (mean 14.28). Encode was 4.97 to 4.99 ms on both and a step 358 to
369 us on both. That is noise. Windows waits on CI; the `Net` code has no
platform branch.

A review pass followed. It dropped the default argument on the virtual
`linear`, since no virtual in eacp or WhisperEACP has one; grouped
`KernelNet`'s parallel vectors into small structs; fixed `prepare()` not
clearing the column buffer on a re-prepare; and made the flush-before-allocate
ordering structural rather than a convention: an op is dispatched before the
next one takes a scratch slot, which is what makes reusing a slot safe.

Phase 2 findings:

- **A weight has to know its shape.** A `Net` op reads a weight's extents
  rather than being told them, so `TensorBuffer` gained the shape the
  safetensors header gives it, and `Weight` is `TensorBuffer`. The graph
  backend wants the same shape for the blob.
- **The op list moved in five places.** `rows` and `transpose` are both views
  on the kernel backend, a byte offset and a stride swap with no dispatch;
  `add` consumes its stream as
  `linearAdd` does; `appendLinear` returns the cache it appended to, beside a
  new `cached` that reads one back and `makeCache` that sizes one; `argmax`
  left the seam, since `Whisper` dispatches it and the net does not; and
  `linear` lost its default activation.
- **A `Tensor` is a ref-counted handle, and liveness places it.** A value
  takes the lowest-numbered free scratch buffer of exactly its capacity,
  reserved at prepare, and gives it back when its last copy goes, so a body
  scopes its intermediates like any other value and a recording allocates
  nothing. A `Binding` carries the capacity of what it points at, the mel's
  3000 frames or the token slots' 448, so a value derived from it is sized for
  the largest run rather than this one.
- **`output()` costs no copy.** An op is dispatched when the next one is
  recorded, not when it is called, so `output()` can still point the op that
  made a value at the caller's buffer: the encoder's last layer norm lands in
  the encoder rows, and the logits in the caller's logits buffer.
- **The barriers are derived.** A dispatch that reads a buffer written since
  the last barrier, or writes one read or written since it, is preceded by
  one. That reproduces the hand-placed set exactly, the three barrier-free
  q/k/v projections and `beginSequence`'s none included.

### Phase 3 - the Core ML encoder

The Core ML `Net` backend, the encoder built through it at prepare, the
enumerated audio-context shapes, the backend's own layout transposes, the seam
copies, `Tests/Net`, the oracle contestant, the benchmark column, the live
transcriber running the encoder on the engine. Done when the transcript
matches on the test audio, the row-by-row tolerance is measured and written
down, and the benchmark shows the encoder's wall time on the engine beside its
Metal time, with the GPU idle during it.

Status as of 2026-09-24: done on a Mac, committed in both trees, eacp as
`5e7849c5` on this branch and WhisperEACP as `13711f7` on its `EDSL-CoreML`,
on top of `ec0b7c0`. eacp came
first, and WhisperEACP then needed nothing further from it. In eacp, G1 to G5
under "Gaps for eacp" closed in `ML/Graph` and `ML/Model`, and `Tests/ML` holds
the ground truth: `WhisperEncoder.h` builds the tiny.en encoder at its real
sizes over seeded weights (80 bands, width 384, fc 1536, six heads, four
layers, the eighteen contexts with 1500 the default, the key projection without
a bias, the positional add through `sliceLike`, fused attention, fp16 rows
out), `MLGraphTests` builds and compiles it enumerated and fixed at 448, and
`MLTests`' `MLEncoder` suite runs it against an fp32 scalar reference of the
same graph (`EncoderReference.cpp`) at 1500, 448 and 576 rows under every
setting, times it and reads its plan; `MLGraphTests` runs 82 tests and
`MLTests` 38, all passing with and without `EACP_REQUIRE_ANE=1`, `MLTests` in a
minute rather than eight seconds, three engine compiles being most of it. In
WhisperEACP, `CoreMLNet` (`Lib/WhisperEACP/Net`, in `whisper-net`, which now
links `eacp-ml-graph` and so builds on every platform) records
`recordEncoder`'s calls into a `Graph`: the mel as an fp16 `[1, 80, 2n]` input
enumerated over the eighteen contexts, the one transpose a relabelling, the
convolutions' channels-first output turned into `[?, 384]` rows before anything
else reads it, `rows` a lazy prefix that `add` lowers to `sliceLike`, attention
split into `[6, ?, 64]` heads around the fused op (or two `matmul`s and a
`softmax` where the OS stops at specification 8), and every weight a blob
constant named after its key in the safetensors file, which `TensorBuffer` now
carries. The cache keys the weights by eacp's own SHA-256 over the blob
(`weightsName` left empty), so a fine-tuned checkpoint with tiny.en's header to
the byte, and an F32 and an F16 file that narrow to the same blob, key as their
bytes say. `CoreMLEncoder` (`Lib/WhisperEACP/Encoder`, Apple only, behind
`if (TARGET eacp-ml)`) records, builds and loads that model and runs it
blocking or through `encodeAsync`, the mel copied in through the strided
`copyFrom` and the rows widened out through the packed `copyTo`. `Whisper`
chooses it before `prepare` with `setEncoderBackend(coreML)` (supported where
the build has `EACP_HAS_COREML`, `isSupported()` holds and specification 8
loads), `setEncoderComputeUnits` (`cpuAndNeuralEngine` by default) and
`setEncoderCacheDirectory`, and reports it through `encoderWasCacheHit`,
`encoderLoadSeconds`, `lastEncoderPredictSeconds`, `lastEncoderMelSeconds` and
`encoderComputePlan`; under it `prepare` builds host weights and compiles and
allocates nothing of the kernel encoder, and an encode commits the mel alone,
waits and predicts. `transcribeAsync(samples)` returns
`Threads::Async<Vector<TokenId>>`: under Core ML it commits the mel on the
calling thread, calls `encodeAsync` and decodes in that Async's resolve on the
main thread; on the kernels it is `transcribe()`, resolved before it returns,
and `transcribe()` is the same body split into `beginRun`, the encode and
`decodeTranscript`, with the dispatches unchanged. A run in flight
(`isTranscribing()`) makes `transcribe`, `transcribeAsync`, `setAudioContext`
and `prepare` a `logic_error`. `LiveTranscriber` runs through it: while a run
is out `update()` starts nothing and takes no audio, the resolve applies the
result where the blocking call did, a failure throws from the next `update()`,
`flush()` waits through `Async::waitFor`, `clear()` drops a late result by
generation, and a weak token keeps a result off a destroyed transcriber;
`LiveStats` gained `runInFlight` and `runsThatChangedTheText`. `Benchmark`
gained its `WhisperEACP ANE` column with `--units=` and `--plan`, and `--live`
streams on both backends; `DeviceInfo` prints what Core ML offers and, with
`--plan`, where it places the bundled model's encoder; `Apps/Demo/LiveTranscribe`
gained `--coreml`. The suite went from 316 at phase 2 to 355 in Debug, all
passing, and `NetTests`, `WhisperTests` and `OracleTests` pass under
`EACP_REQUIRE_ANE=1` as well. The oracle's bar holds: on the engine, with the
fused attention, the tokens are whisper.cpp's exactly on jfk.wav, 24 at the
full window and 23 at 576, and the kernel path's; under `cpu`, the placement
the macOS CI runner gives every setting, they are whisper.cpp's exactly as
well.

A review pass followed. It made a `prepare` that fails leave the `Whisper`
unprepared, where a failed re-prepare had left the earlier decoder beside no
encoder, and made `prepare` with a run in flight a `logic_error`; declared the
Core ML encoder after the buffers its pending run writes and the decoder that
reads them, so that its destruction, which abandons the run, comes first; made
the compute-unit mapping a named function; added `setEncoderCacheDirectory`,
so that `NetTests`, `WhisperTests` and `OracleTests` share one fixed cache,
`<temp>/whisper-eacp-tests/CoreML`, where each had compiled the same model
into its own; resolved `transcribeAsync` outside the decode's `try`, so a
caller's continuation that throws is not taken for a failed decode; and made
`LiveTranscriber::flush()` report a timeout as one, leaving that run in flight
to land on a later `update()`, and stop throwing for a failed run that
`clear()` had already dropped.

The seeded encoder in `MLTests`, on the phase 1 machine: max abs error at
1500 / 448 / 576 rows (max rel is left out: an encoder row is mostly values
near zero, and it runs to 10 and more everywhere), the device the plan
reports, and the median of nine predictions after the first:

| setting | placed | max abs error | 1500 rows | 448 rows |
| --- | --- | --- | --- | --- |
| CPU | CPU | 4.9e-2 / 3.2e-2 / 3.5e-2 | 18.6-19.0 ms | 4.5-4.6 ms |
| CPU and GPU | GPU | 6.4e-3 / 5.8e-3 / 6.5e-3 | 3.1-5.1 ms | 1.6-2.2 ms |
| CPU and engine | engine | 2.2e-2 / 2.3e-2 / 2.3e-2 | 11.3-11.4 ms | 1.4-1.5 ms |
| all | GPU | as CPU and GPU | 2.4-6.2 ms | 1.8-2.9 ms |

The tolerances sit at about three times these: 0.15 on the CPU, 0.02 on the
GPU, 0.07 under the two engine settings, which are held to the CPU's bound
unless `EACP_REQUIRE_ANE=1`, because reading an engine plan costs a compile.

With tiny.en's own weights, measured by `NetTests` in a Debug build on the
phase 1 machine, over jfk.wav's mel, against the kernel encoder's fp32 rows at 1500 / 576 / 448 positions,
with the warm prediction (the second of two at that context, over several
runs) beside it:

| setting | max abs | 99.9th percentile | mean abs | predict |
| --- | --- | --- | --- | --- |
| CPU and engine | 0.26 / 0.21 / 1.10 | 0.056 / 0.069 / 0.072 | 0.0076 / 0.0091 / 0.0090 | 11.3-11.5 / 2.5-2.7 / 1.6 ms |
| all (GPU) | 0.031 / 0.055 / 0.15 | 0.0084 / 0.011 / 0.010 | 0.0011 / 0.0015 / 0.0014 | 5.5-6.5 / 3.2-3.7 / 3.2-3.9 ms |
| CPU | 0.42 / 0.20 / 2.60 | 0.14 / 0.070 / 0.098 | 0.018 / 0.010 / 0.010 | 18.3-19.4 / 6.2-6.7 / 4.4-4.8 ms |

The tests hold each column at about three times its worst: 3.5, 0.22 and 0.03
on the engine, 0.5, 0.035 and 0.005 on the GPU, 8, 0.45 and 0.06 on the CPU;
every setting is held to the CPU's unless `EACP_REQUIRE_ANE=1`, since CI
places everything on the CPU. The same encoder cut after 0 to 4 layers, one
fixed program per depth, puts the engine's max abs at 0.064, 0.15, 0.21, 0.24
and 0.26 at 1500 positions, and the GPU's at 0.0069, 0.024, 0.028, 0.023 and
0.031. In Debug, `CoreMLEncoder::encode`, the two seam copies and the
prediction, took 27-29 ms warm at 1500, 11-13 at 576 and 9-11 at 448, the
unoptimised copies being most of it. The compute plan placed all 39 of the
encoder's `conv`, `linear`, `layer_norm` and `scaled_dot_product_attention`
ops on the engine under CPU and engine.

The benchmark splits the Core ML column's encode three ways: `mel on the GPU`
(`Whisper::lastEncoderMelSeconds()`, the mel's own command buffer, committed
and waited on), `predict`, and `seam copies`, the encode less the other two.

Release, on the phase 1 machine (M5 Max), jfk.wav, 30 timed runs after one
warm-up, the encoder on the engine under CPU and engine unless the column
says `all`; the plan put all 90 of the program's ops on the engine (and all
90 on the GPU under `all`), and the transcript was the reference's at every
context but 448, where all four columns loop, since jfk.wav's 11 s does not
fit 448 positions (8.96 s):

| context | Metal transcribe | ANE transcribe | Metal encode | ANE encode | mel / predict / seam | Metal decode | ANE decode |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1500 | 14.18 ms | 21.45 ms | 4.99 ms | 11.84 ms | 0.39 / 11.26 / 0.16 ms | 9.00 ms | 9.48 ms |
| 1500, `all` (GPU) | 14.22 ms | 11.28 ms | 4.98 ms | 2.07 ms | 0.38 / 1.51 / 0.18 ms | 9.04 ms | 9.04 ms |
| 704 (`audio`) | 11.09 ms | 12.60 ms | 2.39 ms | 3.61 ms | 0.36 / 3.11 / 0.13 ms | 8.50 ms | 8.84 ms |
| 576 | 10.32 ms | 11.50 ms | 2.17 ms | 2.93 ms | 0.37 / 2.43 / 0.13 ms | 7.97 ms | 8.41 ms |
| 448 | - | - | 1.99 ms | 2.12 ms | 0.41 / 1.57 / 0.14 ms | 409 us/step | 409 us/step |

whisper.cpp at 1500 was 30.54 ms on Metal and 99.21 ms on the CPU, encode
4.88 and 72.17 ms. The engine load was a cache hit at 0.03-0.04 s, a whole
`prepare` 0.14-0.16 s; the first load of the session, after the key changed,
compiled in 13.55 s. The same build ten minutes earlier gave the same
numbers to within 0.1 ms.

The live figures, `Benchmark --live`: jfk.wav on repeat with a 1.5 s gap,
33 ms ticks. The one quiet-machine figure is a 15 s stream at the whole
window, 25 runs a side: 21.3 ms a run on the kernels (encode 14.9, decode 6.1)
and 26.6 ms on the engine (encode 14.2, decode 12.3). **The 30 s table below
was taken under load**, with `mediaanalysisd` and `spotlightknowledged` holding
about 180% CPU each and the load average near 20, and on four repeats its
engine column ranged 61-106 ms a run; it says what the engine does on a busy
machine, not what it costs on a quiet one. The load average was still 10 to
30 through the final pass, so the table was not retaken:

| | kernels, window | engine, window | kernels, `audio` | engine, `audio` |
| --- | --- | --- | --- | --- |
| runs (changed the text) | 49 (39) | 41 (37) | 49 (41) | 43 (35) |
| per run, mean / longest | 22.9 / 37.4 ms | 106.3 / 333.2 ms | 18.3 / 29.6 ms | 89.1 / 652.2 ms |
| encode / decode, mean | 15.6 / 6.8 ms | 54.9 / 37.4 ms | 10.0 / 7.8 ms | 46.7 / 22.0 ms |
| duty | 3.7% | 14.5% | 3.0% | 12.8% |
| segments committed | 2 | 2 | 2 | 2 |

On the engine a run is its start to its result, loop turns included, so its
`per run` and `duty` are latency, not main-thread or GPU time.

The "done when", clause by clause. The transcript matches on the test audio:
yes, on the engine, on the GPU and on the CPU; the oracle's tokens are
whisper.cpp's under CPU and engine and under `cpu`, `Tests/Net` and the four
live twins hold the engine's to the kernels', and the benchmark prints `same`
under `all` as under CPU and engine. The row-by-row tolerance is measured and
written down: yes, in the second table above, and `Tests/Net` holds it. The
benchmark shows the encoder's wall time on the engine beside its Metal time,
with the GPU idle during it: yes, and the idle GPU is shown by construction
rather than by a counter, since on the Core ML path the mel row is the only
GPU work in an encode, its own command buffer committed and waited on before
the prediction starts, `--plan` puts all 90 ops on the engine, and the
decoder's first step is not recorded until the rows are back. Phase 3 is done.

What the numbers say about the engine is not what the plan expected: it is
slower than the Metal kernels at every context, and what it buys is an idle GPU
and an idle main thread for the length of the prediction. The compute-unit
default, `cpuAndNeuralEngine`, is a decision these numbers now put in question,
and it is left to the author.

Phase 3 findings:

- **`all` puts the encoder on the GPU, so the default is CPU and engine.** On
  this machine, enumerated or fixed at 448 or 1500, every op of the encoder
  lands on the GPU under `ComputeUnits::all`, as under CPU and GPU; only
  `cpuAndNeuralEngine` reaches the engine, where every op lands, `shape` and
  `slice_by_index` included, so nothing falls back to the CPU. Phase 1's
  1500-row `linear` and `softmax` went to the engine under `all`; the encoder's
  op mix does not. So `CoreMLEncoderOptions` and `Whisper` default to
  `cpuAndNeuralEngine`, the only setting that leaves the GPU idle, and the
  placement test asserts the engine under it and only logs `all`.
- **The engine is slower than the Metal kernels at every context.** Its
  prediction alone against the kernels' whole encode, the mel included, in
  Release: 11.26 against 4.99 ms at 1500, 3.11 against 2.39 at 704, 2.43
  against 2.17 at 576, and level at 448 (1.57 plus a 0.41 ms mel against 1.99).
  The plan's premise, that the engine would lose the full window and win the
  short live contexts, did not hold: it scales with the context more steeply
  than the kernels (7.2 times from 448 to 1500, against 2.5), eightfold for 3.3
  times the rows, which points at the 1500 by 1500 attention scores, so any
  crossover is at or below 448, the floor. What the engine buys is an idle GPU
  and an idle main thread for the length of the prediction, not wall time.
- **Core ML on the GPU is the fastest encoder here.** Under `all` it predicts
  the full window in 1.51 ms, and its encode is 2.07 ms against the kernels'
  4.99 ms, 11.28 ms a transcribe against 14.22, with the same tokens: Apple's
  own GPU lowering of the same program is about three times faster than our
  Metal kernels. That is a target for the kernels (the attention and the
  projections at 1500 rows), and a reason `EncoderComputeUnits::all` is worth
  exposing as a setting rather than as a diagnostic.
- **The engine compile is 13.5 s, once per cache directory.** A cold load of
  the enumerated model under CPU and engine took 13.5 to 14.1 s, the engine
  compiling all eighteen members; a warm one 12.6 to 13.1 ms in eacp's tests
  and 30 to 165 ms through `CoreMLEncoder`, recording and building the program
  not included (recording tiny.en is 0.53 s in Debug), and a whole warm
  `prepare` on Core ML is 0.14 to 0.16 s in Release. Under `all` it is 0.75 to
  0.8 s cold and 20 ms warm. The compile to `.mlmodelc` itself is under 0.1 s.
  The cache key is the program, the weights and the OS, not the units, so the
  compile is paid once per model whichever setting asks first; but it is paid
  once per cache directory, and with the default, `appCacheDirectory() /
  "CoreML"`, `DeviceInfo`, the benchmark, each test binary and the demo each
  paid it for the same model until the tests were pointed at one directory. A
  program fixed at one context compiles for the engine in 0.63 s at 448 and
  1.02 s at 1500, so the risk's fallback, one model per context compiled on
  first use, is about a second at each context's first use against fourteen up
  front.
- **The compute plan read costs the compile again, the first time.** Reading
  it under CPU and engine took 13.2 to 14.7 s the first time in a session,
  however warm the cache, and 0.0 s right after another read of the same
  compiled model (0.8 s and 0.1 s under `all`): the OS appears to keep the
  compile it did for the plan for a while. A program fixed at one context
  reads its plan in about a second. The benchmark and `DeviceInfo` read it
  only under `--plan`.
- **The compute plan is the program's, not a member's.** `MLComputePlan` is
  loaded from the compiled model and takes no shape, and the enumerated
  model's plan lists the `shape` op a member fixed at its size would not hold:
  it places the program as written, once. The probe per member is a program
  fixed at that context (G4): at 448 and at 1500 every op lands on the engine
  under CPU and engine, and the enumerated model's 448 member runs at the fixed
  program's speed, 1.42 ms against 1.41. The op mix placed phase 1's lone
  448-row `linear` and `softmax` elsewhere, not the size alone.
- **The fp16 error is arithmetic throughout, not layer norm drifting.** Against
  tiny.en's own weights the engine's max abs at the full window is 0.26, ten
  times the seeded encoder's 2.2e-2, where the GPU through Core ML stays near
  its seeded number (0.031). Cut by depth, the gap is there before the first
  layer: the convolutions, the positional add and the final norm alone are
  0.064 on the engine against 0.0069 on the GPU, the first layer doubles it,
  and it grows little after (0.15, 0.21, 0.24, 0.26). The oracle's tokens are
  unchanged by it.
- **The maximum is a handful of elements.** At 448 positions the engine's max
  abs is 1.1 and the CPU's 2.6, where the 99.9th percentile is 0.072 and 0.098
  and the mean 0.009 and 0.010; it appears only after the fourth layer, and on
  the GPU too (0.15), so it is the model amplifying one element rather than a
  device fault. A tolerance on the maximum alone would either be loose or
  fail, so `Tests/Net` holds the tail and the mean as well.
- **The engine is shared with the system.** With `mediaanalysisd` and
  `spotlightknowledged` busy, the back-to-back benchmark held its numbers
  (11.26 ms), but the live stream's engine runs went from 26.6 ms to 61-106 ms,
  with a longest of 0.3 to 0.65 s, while the kernel column moved by 1-2 ms. A
  run that waits for the engine waits for whatever else the OS has queued on
  it, and the live path, which idles it between runs, sees that most.
- **The decode after an engine encode is slower.** Back to back it is about 5%
  slower (9.48 ms against 9.00 at the window, 8.41 against 7.97 at 576), in
  the quiet live stream twice as slow (12.3 ms against 6.1 for the same steps),
  and three to five times under load. With the engine the GPU sees a 0.4 ms mel
  and then nothing for the length of the prediction, where the kernels' encode
  keeps it busy up to the first step; that is consistent with the GPU clocking
  down in between, though nothing here measures the clock.
- **A member's first prediction is slow.** The enumerated model's first
  prediction at 448 rows took 10.7 to 11.8 ms on the engine and 42 ms on the
  GPU, the ones after it 1.4 and 1.8 to 2.9; a fixed 448 program's first is
  1.6 ms. `LiveTranscriber` primes nothing, so the first run at each new
  context pays it.
- **Core ML refuses an output that folds away.** An output that is an input
  cut to its own shape fails the load with "Failed to build the model
  execution plan ... error code: -6", so `sliceLike(x, x)` is `x`; a program
  whose outputs were the enumerated cut and a constant table cut to a fixed
  input nothing else read failed the same way, and passes once each cut is
  added to its input, as the encoder's is.
- **The reference is cheap enough to keep in the suite.** The fp32 encoder,
  written as rows times transposed weights so the inner loops vectorize and
  compiled at `-O2` in a Debug build, takes 1.5 s at 1500 rows and 0.33 s at
  448.
- **In Release the seam is noise.** The copies cost 0.13-0.18 ms and the mel's
  own command buffer 0.36-0.41 ms at every context, so an encode on Core ML is
  its prediction plus about half a millisecond. In Debug the unoptimised
  copies were most of a 27 ms encode around an 11.4 ms prediction, which was a
  cost of the build, not of the seam.

### Phase 4 - the decoder, if the numbers say so

Phase 3 measured the per-prediction overhead as the encode less the
prediction, 0.5-0.6 ms (the mel's command buffer 0.36-0.41 ms and the seam
copies 0.13-0.18 ms, Release, at every context), on top of a smallest engine
prediction of 1.57 ms (the whole encoder at 448), against a Metal decode step
of 332-409 us all in; a Core ML decode step would have to fit its
prediction, its hop and its copies in that.

Only if phase 3's per-prediction overhead, measured, leaves room for a
decode step to win. Stateful model with `MLState` for the KV cache, fixed
`maxPositions` with a mask, the argmax inside the model, the token carried as
state. Not planned in detail until phase 3 reports.

Status as of 2026-09-24: the measurement this phase was gated on is taken, and
nothing of the phase itself is built. `Tests/ML/WhisperDecoderStep.h` builds
one tiny.en decode step at its real sizes over seeded weights (width 384, six
heads, four layers, fc 1536, the 51864-token vocabulary, 448 positions,
structure read off WhisperEACP's `recordDecoderStep`): the token and its
position embedded by two `gather`s of int32 inputs; per layer the
self-attention norm, the query, key (no bias) and value projections, the key
and value rows concatenated after the layer's `[448, 384]` slice of the cache,
`matmul`, the scale and an additive mask in one `apply`, `softmax`, `matmul`
and the output projection; the cross-attention norm, the query projection
and the fused, unmasked `scaledDotProductAttention` over the layer's
`[1500, 384]` keys and values; the MLP norm, fc1, `gelu` and fc2; then the
final norm and the logits as a bias-free `linear` against the token embedding
itself, one blob tensor feeding both the gather and the logits as
`logitsWeight()` ties them. It is stateless: the inputs are the token, the
position, a `[1, 448]` fp16 mask (0 for a valid cache row, -10000 for the
rest) that a constant 0 for the step's own key extends to 449 keys, the
self-attention caches as `[4, 448, 384]` and the cross keys and values as
`[4, 1500, 384]`, all fp16, 12 MB a prediction; the outputs are the fp16
logits row and the `[4, 384]` key and value rows the step would append. The
self-attention is spelled with `matmul` and `softmax` because
`scaledDotProductAttention` takes only a causal flag, and a causal mask over
one query row masks nothing; the mask is an ordinary add before an ordinary
softmax, not the fused op's `attn_mask`, so the float mask Core ML's fp16
attention ignores (under "Decisions taken now") does not come into it, and the
reference agrees at a prefix of 1, where an ignored mask would attend to 447
rows of noise. The program is 196 ops, specification 9 for the cross-attention,
over a blob of about 57 MB, 40 of it the embedding. A second program is the
same step with the four caches baked into the blob, so that only the token,
the position and the mask cross into a prediction: a bound on what a stateful
step, whose caches never leave Core ML, could cost. It computes the step for
the suite's one set of caches only. `MLGraphTests` builds and compiles the
first (`MLGraph/DecoderStep`); `MLTests`' `MLDecoderStep` suite runs both
under every setting against an fp32 scalar reference of the same step
(`DecoderStepReference.cpp`, built from the encoder reference's pieces, 27-39
ms a step), reads the first one's plan, and times them. `MLGraphTests` runs
83 tests and `MLTests` 44, all passing with and without `EACP_REQUIRE_ANE=1`.

Decision, 2026-09-25: phase 4 is not built. The criterion is the plan's own,
and the table below answers it: no setting fits a decode step in the Metal
step's time before its hop and its copies, and the resident-cache bound says
state would not change that. G8 to G10 stay open as what a phase 4 would need,
should a machine or an OS move the numbers; `Tests/ML/DecoderStepTests.cpp` is
the measurement to rerun then. G11, which the measurement surfaced, was an eacp
bug in its own right and is closed under the gaps.

Release, on the phase 1 machine (M5 Max, macOS 27.0), the blocking
`predict()` with every output bound, median and minimum of 25 predictions
after five warm-up ones, the range over seven runs of the suite (five for the
resident caches, six for `predictAsync`); max abs error
over prefixes 1 and 447, the logits and the appended rows. The machine was
under load throughout, `mediaanalysisd` at about 220% CPU,
`spotlightknowledged` at about 140% and another agent's benchmark beside
them, the load average 8 to 23:

| setting | placed | max abs, logits / rows | predict at prefix 1, median / min | at prefix 447 | caches resident, prefix 1 / 447 median | `predictAsync` to its resolve, median / min |
| --- | --- | --- | --- | --- | --- | --- |
| CPU | CPU | 7.9e-2 / 3.0e-2 | 0.63-0.68 / 0.59-0.63 ms | 0.63-0.66 / 0.58-0.62 ms | 0.63-0.67 / 0.62-0.65 ms | 1.6-2.9 / 0.91-0.95 ms |
| CPU and GPU | GPU | 8.5e-3 / 3.4e-3 | 2.57-2.63 / 2.48-2.57 ms | 1.85-2.69 / 1.47-2.43 ms | 1.10-1.59 / 1.07-1.20 ms | 5.2-6.0 / 3.1-3.4 ms |
| CPU and engine | engine, but the two `gather`s on the CPU | 2.8e-2 / 1.3e-2 | 0.76-1.18 / 0.74-0.93 ms | 0.76-1.15 / 0.74-0.93 ms | 0.76-0.77 / 0.77 ms | 1.9-2.4 / 1.0-2.0 ms |
| all | GPU | as CPU and GPU | 1.48-1.94 / 1.28-1.47 ms | 1.13-1.29 / 1.06-1.16 ms | 1.05-1.07 / 1.04-1.06 ms | 4.4-5.7 / 2.9-3.4 ms |
| phase 3, for comparison | | | Metal decode step, all in: 332-409 us | | | seam copies 0.13-0.18 ms, mel 0.36-0.41 ms |

The tolerances sit at about three times these: 0.25 on the CPU, 0.03 on the
GPU, 0.09 under the two engine settings, which are held to the CPU's bound
unless `EACP_REQUIRE_ANE=1`, as the encoder's are. The first prediction after
a load was 2.3-2.5 ms on the CPU, 1.6-2.5 ms on the engine and 27-39 ms on the
GPU. Each run starts from an empty cache directory, so its first load compiled
the package, in 0.13-0.52 s; the first load under CPU and engine in a run, a
cache hit, took 0.55-0.67 s, the engine compile, and 28-48 ms after that; the
other hits took 17-170 ms, and the plan read 0.05-0.8 s.

Phase 4 findings:

- **No setting fits a decode step in the Metal step's time.** The fastest
  prediction, the CPU's, has a median of 0.63-0.68 ms and never went below
  0.58 ms, 1.5 to 2 times a whole Metal step (332-409 us) before any hop or
  copy; the engine's median is 0.76-1.18 ms, two to three times; Core ML on
  the GPU 1.1-2.7 ms. The hop back to the main thread
  and phase 3's seam copies only add to that. On this phase's own criterion,
  a Core ML decode step fitting its prediction, its hop and its copies in
  332-409 us, a stateless step does not fit on this machine under any
  setting, and the resident-cache bound says a stateful one would not either:
  the CPU is unchanged at 0.62-0.67 ms, the engine steady at 0.76-0.77 ms.
- **The 12 MB of caches costs the GPU and not the others.** With the caches
  resident the GPU settings drop from 1.1-2.7 ms to 1.04-1.59 ms, the engine
  loses its spread (0.76-1.18 to 0.76-0.77 ms) and the CPU does not move. So
  what `MLState` could save is at most the difference between those columns;
  what is left is the program's own compute and Core ML's per-prediction cost,
  which phase 1 put at 0.14-0.23 ms for a lone projection.
- **The mask costs nothing.** On the CPU and the engine the prefix-1 and
  prefix-447 times are within run-to-run noise, as they should be for a fixed
  program. On the GPU settings prefix 447 was the faster in nearly every run,
  the resident program's too, where no cache crosses at all; it is timed
  second, after a first prediction of 27-39 ms, so this reads as the
  GPU still settling after five warm-up predictions rather than as the mask.
- **The engine takes the step but not its `gather`s.** Under CPU and engine
  every op lands on the engine but the two embedding lookups, which stay on
  the CPU, so every prediction starts on the CPU and hands over; under `all`
  everything goes to the GPU, as the encoder does. The plan of this fixed
  program reads in under a second, as phase 3 found for fixed programs.
- **The hop, as measured here, is one to four milliseconds.** From
  `predictAsync()` to its continuation on the main thread took 0.7-1.6 ms more
  than the blocking prediction at the median on the engine, 1.0-2.3 ms on the
  CPU and 2.5-4 ms on the GPU settings, though the loop here sits idle for
  16.7 ms between predictions, which may leave the devices clocked down, so
  it overstates a decoder that keeps them busy. `Async::waitFor` returned
  16.66 ms after every call, whatever the setting, where the continuation had
  run 1-6 ms in: the nested pump on macOS exits a frame after the resolve,
  not on it (G11). With G11 fixed the hop was remeasured, in a Debug build,
  on an idler machine: `predictAsync()` to its resolve took a median of 0.70
  ms against the blocking prediction's 0.66 ms on the CPU, 1.19 against 1.18
  ms under CPU and GPU, 0.81 against 0.79 ms under CPU and engine and 1.15
  against 1.23 ms under `all`, so the hop is now within the noise of the
  prediction, and `waitFor` returned 5-15 us after the resolve.
- **The error is of the seeded encoder's order.** The engine's max abs on
  the logits is 2.8e-2 against the seeded encoder's 2.2e-2, the GPU's
  8.5e-3 against 6.4e-3 and the CPU's 7.9e-2 against 4.9e-2. Whether it moves
  an argmax is not tested here: the seeded weights make no meaningful token,
  and phase 3 found tiny.en's own weights ten times the seeded error on the
  engine.

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
- The macOS CI lane has no Neural Engine to place on: the `macos-26-arm64`
  runner placed every op on the CPU under every setting in phase 1's CI run.
  So placement is asserted only on a developer's Mac, a change that moves the
  encoder off the engine is caught by the benchmark rather than by CI, and
  `build.yml` must not set `EACP_REQUIRE_ANE`. The lane's CPU path on macOS
  26 traps on the enumerated encoder (G12).
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

Phase 2 surfaced one more:

- `GPU::ComputePass` has no way to observe what it recorded, so the check that
  phase 2 left the dispatches and barriers unchanged had to fake a Metal
  compute encoder. A recording or counting hook on the pass would let a
  portable test pin dispatch and barrier counts on every backend. Still open.

Phase 3 surfaced these, in eacp before WhisperEACP was touched:

- G1: the positional add over an enumerated context had no spelling. `slice`
  refused a fixed axis with an unknown end, and `apply` a `[1500, 384]` table
  against `[?, 384]` rows. Closed in this phase: `sliceLike(x, reference)`,
  lowered to `shape` and `slice_by_index` where the reference enumerates,
  which Core ML places on the engine with the rest.
- G2: `linear` took a bias tensor always, so Whisper's bias-less key
  projection meant a named zero constant per layer. Closed in this phase:
  `linear(x, weight)` supplies an unnamed zero blob constant, which reaches
  the blob only when the linear reaches an output.
- G3: the seam copies were packed from offset 0, and the mel binding is
  `[80, 3000]` read 2N frames a band. Closed in this phase: `copyTo` and
  `copyFrom` take a byte offset and a row stride in bytes.
- G4: `computePlan()` describes the compiled program, not a member, and under
  the engine settings reading it costs the engine compile again. Not
  closable in eacp, since `MLComputePlan` takes no shape; the probe is a
  program fixed at each context, in `Tests/ML/EncoderTests.cpp` rather than
  the library, and it found 448 and 1500 wholly on the engine. Still open as
  a Core ML limit.
- G5: `isSupported()` checks macOS 13, but every program the graph writes is
  specification 8 (macOS 14) or, with `scaledDotProductAttention`, 9 (macOS
  15), so it answered yes on OSes that load none of them. Closed in this
  phase: `supportsSpecification(version)`, asked with what
  `specification()` reports.

WhisperEACP's side of phase 3 surfaced two more:

- G6: the default compiled-model cache is per app
  (`appCacheDirectory() / "CoreML"`), so every binary of one product compiled
  the same model for the engine, about 13.5 s each. What the callers wanted
  was a shared directory, and `Options::cacheDirectory` already allows one:
  WhisperEACP passes it through `CoreMLEncoderOptions::cacheDirectory` and
  `Whisper::setEncoderCacheDirectory`, and its three Core ML test suites now
  share one fixed directory under the temp directory and compile once per
  machine. What remains open is smaller: the default has no level above the
  app, so a product's binaries share only if each names the directory, and
  nothing evicts a compiled model once no program or weights key to it, so a
  long-lived shared directory only grows.
- G7: `CoreMLEncoder`, and so `Whisper::transcribeAsync`, cannot time the
  prediction on its own once it goes through `predictAsync`: the Async gives
  the result, not how long the queue waited or the prediction ran, so an
  async encode's time includes the hop back to the loop. Closed in this
  phase: `Prediction::queueWaitSeconds`, from the `predictAsync()` call to the
  moment the job has the model to itself, and `Prediction::predictSeconds`,
  the `predictionFromFeatures:` call alone, both taken on the model's queue
  as `double` seconds, since `Time::MS` counts whole milliseconds and a small
  prediction runs in less. The blocking `predict()` still returns a `Result`:
  its caller is on the thread that runs it and can time it with no hop to
  leave out.

Phase 4's measurement surfaced these, G8 to G10 for a phase 4 that goes ahead
and G11 for eacp itself; G12, which CI surfaced after it, is Apple's:

- G8: `Graph` has no argmax, so a step's output is the whole logits row,
  51864 fp16 values in a padded 103744-byte row, read back and reduced on the
  host every token, where the kernel path's argmax stays on the device. MIL's
  `reduce_argmax` is the op, with an int32 output. Still open.
- G9: `scaledDotProductAttention` takes only a causal flag, so attention over
  a fixed cache with a run-time valid prefix has no fused spelling; the step
  builds it from `matmul`, an additive mask in `apply`, `softmax` and
  `matmul`. A form taking a mask tensor, lowered to a bool `attn_mask` the way
  the causal one is (since Core ML's fp16 attention ignores a float one),
  would give the engine the fused op; whether Core ML honours a bool mask
  computed from an input rather than a constant is unmeasured. Still open.
- G10: nothing in eacp is stateful. `Graph` has no state input (MIL's
  `read_state`, `coreml_update_state`, specification 9), and `Model` no
  `MLState` handle to make per sequence and pass to a prediction (macOS 15,
  iOS 18), so the KV cache can only cross as inputs, 12 MB a step for
  tiny.en. The resident-cache program bounds what that would buy (the table
  above). Still open.
- G11: `Async::waitFor` on macOS returned 16.66 ms after every
  `predictAsync()` in the step suite, whatever the setting, where the
  continuation the test chained had run 1-6 ms in. `EventLoop::runFor` waits
  in `nextEventMatchingMask:untilDate:` and `EventLoop::quit` sets its flag
  and posts a wake event, yet the nested pump exits a frame after the resolve
  rather than on it. A caller that waits on each of a run of short Asyncs, as
  a decoder driven through `predictAsync` and `waitFor` would, pays a frame
  per wait. Closed after the phase 4 measurement. The cause: once
  `[NSApp run]` is live, `nextEventMatchingMask:untilDate:` does not hand back
  an event posted with `postEvent:atStart:` until the next display refresh,
  so the nested pump woke a frame late, 16.66 ms on that day's display and
  8.33 ms when remeasured at 120 Hz; the resolve itself, delivered through the
  same wait, was late too, its median 1.0-3.6 ms against 0.70-1.19 ms after
  the fix. The fix: `EventLoop::runFor` on macOS now drains the pending events
  with a `distantPast` date, checks the quit flag and the deadline, and blocks
  in `CFRunLoopRunInMode`, which the wake event `quit()` posts returns from;
  `quit()` is unchanged, and iOS, whose `runFor` never waited in
  `nextEventMatchingMask`, needed nothing. `waitFor` now returns 5-15 us after
  the resolve, and the step suite's median to `waitFor`'s return went from
  8.33 ms at every setting to 0.71 ms on the CPU, 1.20 ms under CPU and GPU,
  0.81 ms under CPU and engine and 1.16 ms under `all`.
  `EventLoop/waitFor/returnsOnResolveNotAFrameLater` in `GraphicsTests`
  resolves from a worker ten times and requires `waitFor` back within 2 ms of
  the resolve; it failed at 4.1-4.6 ms before the fix. It lives there rather
  than beside the other `waitFor` tests in `CoreTests` because `CoreTests`
  runs on nano's default main, outside `[NSApp run]`, where the wait was never
  late.
- G12: on GitHub's macOS runner (macOS 26.6.2, build 25G83, an arm64 VM with
  no Neural Engine, so Core ML places everything on the CPU), the first
  prediction of the enumerated tiny.en encoder under `cpu`, at 1500, dies of a
  SIGTRAP in libBNNS, a `brk` under `BNNSGraphContextExecute_v2`, reached from
  Espresso's `BnnsCpuInferenceOperation` and `MLE5Engine
  predictionFromFeatures:`. It is fatal and cannot be caught, so the library
  cannot guard it at run time. The same tests pass on macOS 27.0 under every
  setting, `cpu` included, and on the runner every fixed-shape prediction
  passes, the 196-op decoder step with its fused cross-attention over
  [1500, 384] among them. What sets the enumerated program apart is the
  enumerated input and the positional add spelled through `sliceLike` (G1).
  Done: `whisperTinyMatchesTheReferenceOnEveryDevice` and
  `whisperTinyLoadAndPredictionTimes` skip their enumerated predictions
  before macOS 27 and log why; `TestMain`'s backtrace on a fatal signal and
  the crash-report artifact in `build.yml` stay. Open: whether a program
  fixed at 1500 traps on 26 too, which
  `MLEncoder/whisperTinyFixedAt1500PredictsOnTheCpu`, not gated on the OS,
  measures on the next run; and what it means for WhisperEACP's
  `CoreMLEncoder` on a macOS 26 machine whose plan lands on the CPU, which
  should refuse or fall back rather than trap once the shape of the bug is
  known.
