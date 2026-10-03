# ML

A Core ML backend for tensor-level compute, beside the compute kernels the
shader EDSL already runs on Metal, D3D12, Vulkan and the CPU. A net written
against eacp keeps its `ComputeProgram` kernels on every platform; this module
is the second way to run the same maths on Apple hardware, as a fixed graph of
whole-tensor ops that Core ML compiles ahead of time and places on the CPU,
the GPU or the Apple Neural Engine. The net that proved it is
[WhisperEACP](https://github.com/eyalamirmusic/WhisperEACP): its tiny.en
encoder and decode step are built in `Tests/ML` at their real sizes over
seeded weights and checked against fp32 scalar references.

It is not a fourth backend of `GPU::ComputePass`. A kernel body is one
thread's view — `threadId()`, `shared<>`, `barrier()`, atomics — and Core ML
has no thread. What the two share is the value layer: an elementwise body
here is written with the EDSL's `GPU::Float` and lowered to MIL rather than to
MSL.

## Two targets

| | |
| --- | --- |
| `eacp-ml-graph` | The graph builder and the writers that turn it into an `.mlpackage`: a MIL program, a hand-rolled protobuf encoder and the fp16 weight blob. Bytes in, bytes out, verified byte for byte against coremltools, with no Core ML under it — so it builds and `MLGraphTests` runs on macOS, Windows and Linux. Links `eacp-core` and `eacp-gpu-codegen` only |
| `eacp-ml` | The runner, Apple-only behind `EACP_HAS_COREML` (`EACP_HAS_GPU` and Apple; a PUBLIC define on the target): compiles a package through a cache, loads it on a chosen set of compute units, predicts synchronously or asynchronously, and hands tensors across to `GPU::Buffer`. Links `eacp-ml-graph`, `eacp-gpu`, Core ML, CoreVideo and Accelerate |

`ML.h` includes the graph and writers everywhere and the runner only where
`EACP_HAS_COREML` is 1, so a caller reaching `ML::Model` on a platform without
it fails to compile rather than to link. Everything is in `eacp::ML`.

## Building a graph

`Graph` records ops as it goes. A failing op returns an invalid `Tensor` and
records why: `isValid()` and `errors()` say so, and `build()` of a graph with
errors is an empty `Package`.

```cpp
#include <eacp/ML/ML.h>

using namespace eacp;

ML::Package projectionAndSoftmax(int rows, const Vector<float>& weights)
{
    auto graph = ML::Graph {};
    auto x = graph.input("x", {rows, 384}, ML::DType::float16);
    auto weight = graph.halfConstant("weight", {384, 384}, weights);
    auto zeros = Vector<float> {};
    zeros.resize(384, 0.f);
    auto bias = graph.halfConstant("bias", {384}, zeros);

    graph.output(graph.softmax(graph.linear(x, weight, bias), -1), "y");

    return graph.build();
}
```

An input is a name, a `Shape` and a `DType`; the four-argument `input` takes
a default shape and a list of enumerated shapes, which is how a decoder step
takes a growing context without a recompile. A `constant` is bytes of any
type, `halfConstant` packs floats to fp16 (`MIL/Half.h` is the conversion),
and `scalar` is one float. Constants that reach an output go into the weight
blob rather than the program text.

The ops are what a transformer needs: `linear` with or without a bias,
`matmul` with either operand transposed, `transpose`, `reshape`, `softmax`,
`sum`, `max` and `argmax` over an axis, `layerNorm` over a set of axes,
`conv`, `gather`,
`concat`, `slice` and `sliceLike`, `gelu`, `cast`, and
`scaledDotProductAttention` either causal or with a run-time mask. The mask
is a float tensor that is lowered to a bool one (`allowed > 0.5`), because
Core ML's fp16 attention ignores a float mask past 32 positions. Anything
elementwise that has no op of its own is `apply`: one, two or three operand
tensors and a body written over `GPU::Float`, exactly as a shader would write
it. The arithmetic, comparisons, `select`, `clamp`, `abs`, `min`/`max`,
`sqrt`/`rsqrt`, `exp`/`log`/`pow`, `tanh`, `erf` and `floor` lower; a call
with no MIL lowering, `sin` say, fails the op with a message naming it.

```cpp
auto body = [](const GPU::Float& value, const GPU::Float& factor)
{ return value * factor + 0.5f; };

auto y = graph.apply(x, scale, body);
```

`toText()` prints the MIL program for a diff against coremltools' output,
`specification()` is the typed form the writers consume, and `build()` is a
`Package` — the model bytes and the weight blob — which `Package::write`
lays out as an `.mlpackage` directory on disk for anything else to open.

## Running it

```cpp
auto model = ML::Model {};
auto options = ML::Options {};
options.units = ML::ComputeUnits::cpuAndNeuralEngine;

if (auto result = model.load(package, options); !result)
    LOG(result.error);

auto input = ML::MultiArray::create({rows, 384}, ML::DType::float16);
auto output = ML::MultiArray::create({rows, 384}, ML::DType::float16);
input.fromFloats(values);

auto inputs = ML::Inputs {};
inputs["x"] = input;
auto outputs = ML::Outputs {};
outputs["y"] = output;

auto result = model.predict(inputs, outputs);
```

`load` takes a `Package`, or a path to an `.mlpackage` (compiled through the
cache) or an `.mlmodelc` (loaded where it lies). `ComputeUnits` is `all`,
`cpuAndNeuralEngine`, `cpuAndGPU` or `cpu`; `units()`, `inputs()` and
`outputs()` report what loaded, and `computePlan()` is Core ML's own account
of where each op was placed and what it costs, which `Apps/ML/Projection`
prints per op. `isSupported()`, `hasComputePlan()` and `hasNeuralEngine()`
answer before anything is loaded.

An output array passed in `Outputs` is bound, so Core ML writes straight into
memory the caller keeps; an output left out is allocated and returned. A
`MultiArray` is a handle: copying one shares its storage. An fp16 array is an
IOSurface-backed pixel buffer, the one form the Neural Engine takes without
converting, zeroed on creation and row-padded to the surface's alignment
(`rowStride()`); fp32 and int32 arrays are plain memory. `toFloats` and
`fromFloats` are the host copies, `copyTo`/`copyFrom` move a tensor to or from
a `GPU::Buffer` packed or at an offset and row stride, converting fp16 to and
from fp32 on the way, and `copyRows` writes one step's rows into a fixed cache
tensor — a key/value cache between decoder steps.

`loadAsync` and `predictAsync` return a `Threads::Async`; jobs run in call
order on the model's own serial queue and resolve on the main thread, so both
are called from there. A `Prediction` carries the outputs plus
`queueWaitSeconds` and `predictSeconds`, taken on the queue. The blocking
forms run on the caller's thread, pump no event loop, and one prediction runs
at a time per model. Destroying the model abandons the Asyncs it handed out.

## The compile cache

Compiling a package is the slow step — a first load of the Whisper encoder
onto the Neural Engine after its key changed took 13.5 s, a hit afterwards
tens of milliseconds — and Core ML keys its own engine cache on the compiled
model's directory. So a compiled model is kept at
`<cacheDirectory>/<hash>.mlmodelc`, where the hash covers the program bytes,
the weights' identity and the OS build, and a hit is never recompiled.
`Options::weightsName` and `weightsVersion` name the weights so a large blob
is not hashed on every run; empty hashes the blob. A fresh compile moves into
place by atomic rename, so two processes racing to the same key both end up
loading one copy; a hit that fails to load twice is treated as damaged, moved
aside and compiled again. `wasCacheHit()` and `compiledPath()` say what
happened, and `defaultCacheDirectory()` is `CoreML` under the app's own
cache folder (`FilePath::appCacheDirectory()`).

## What was measured

Placement depends on problem size and op mix, so there is no one answer. For
the Whisper tiny.en encoder, Core ML on the GPU ran about three times faster
than the EDSL's Metal kernels, and the Neural Engine ran slower than them;
for the decode step the Core ML CPU path was 0.63–0.68 ms against the Metal
step's 332–409 µs, so the decoder stays on the kernels. `plan.md` at the
repository root is the design record with every finding and the gaps still
open.

## Tests and examples

`MLGraphTests` (90 `MLGraph/` cases: shapes, text, protobuf, blob, package,
`apply`, and the encoder and decoder graphs) runs on every lane but iOS, and
where `eacp-ml` exists every package it builds is also compiled and loaded by
Core ML. `MLTests` (55 cases over `MLMultiArray`, `MLAsync`, `MLCache`,
`MLPlacement`, `MLPrograms`, `MLEncoder` and `MLDecoderStep`) needs Core ML;
`EACP_REQUIRE_ANE=1` makes it assert Neural Engine placement, which CI leaves
unset because its macOS runners have none. `Apps/ML/Projection` builds the
graph above, loads it under `--units all|cpu-ane|cpu-gpu|cpu`, prints the
compute plan and times the prediction; `Apps/ML/Spike` is the raw
Objective-C++ spike the module grew out of, kept as a reference for the file
formats.
