# CPU execution of compute kernels — plan

Written 2026-09-24 against `80b93a9a` (branch `cpu-compute-backend`), from a
read of the shader EDSL in `Lib/eacp/GPU/Codegen/` (the graph, the
three-dialect emitter, the builder and value handles, `ComputeProgram` and the
member reflection in `ShaderProgram.h`), `ComputePass`, `Buffer`, the
`eacp-simd` module and `eacp_force_optimization`, the GPU test suites and
their CMake, the compute examples under `Apps/GPU` and the path-coverage
kernels in `GPUWidgets`. Line counts are estimates, not commitments.

## Progress

| Stage | State | Notes |
| --- | --- | --- |
| 0 — the device-free seam | built, macOS green | macOS: `GPUCodegenTests` 104, `GPUTests` 478, `GPUWidgetsTests` 57, `UITests` 183, all passing — the baseline plus the one new case. Linux lanes not yet run (no Docker on the dev machine) — awaits CI |
| 1 — tier one: streams, control flow, 1D/2D/3D | built, macOS green | macOS: `CpuComputeTests` 69 (63 `Executor/`, 6 `Kernel/`), `GPUTests` 478 with CPU halves inside 36 existing cases across 13 suites, `GPUCodegenTests` 104, all passing. Linux and Windows lanes not yet run (no Docker on the dev machine) — awaits CI |
| 2 — tier two: the threadgroup | built, macOS green | macOS: `CpuComputeTests` 91 (64 `Executor/`, 21 `Group/`, 6 `Kernel/`), `GPUTests` 478 with CPU halves in the stage-2 suites and the five stage-1 deferrals, `GPUCodegenTests` 115 (ten `…/runs` and one emitter guard new), `GPUWidgetsTests` 65 (8 new), `UITests` 183, all passing. Found and fixed an emitter bug (stage 2, *as built*). Linux and Windows lanes not yet run (no Docker on the dev machine) — awaits CI |
| 3 — tier three: packed helpers and the SIMD-group matrix | built, macOS green | macOS: `CpuComputeTests` 121 (71 `Executor/`, 21 `Group/`, 14 `Helpers/`, 9 `SimdMatrix/`, 6 `Kernel/`), `GPUTests` 478 with CPU halves in the packed, intrinsic and SIMD-matrix suites and the stage-1 helper deferrals, `GPUCodegenTests` 119 (four `SimdMatrix/…/runs` new), `GPUWidgetsTests` 65, `UITests` 183, all passing. Linux and Windows lanes not yet run (no Docker on the dev machine) — awaits CI |
| 4 — performance | not started | |
| 5 — integration | not started | |

Where the code differs from the sketches below, the code wins; each such
point gets marked *as built* in place, as the last plan did.

## 0. Why

**Realtime.** An app that already runs a kernel on the GPU — an audio plugin
rendering a filter bank, a DSP process doing a convolution, a tool whose
analysis pass is a `ComputeProgram` — wants to run *the same kernel* on the
CPU: synchronously, on the calling thread, inside an audio callback, with no
allocation, no lock and no syscall while it runs, and on a machine or a
platform where no device came up. Today there is no such path. On a Linux box
with no Vulkan driver `Device::isValid()` is false, `ComputeProgram::prepare`
builds nothing, `isValid()` answers false and `ComputePass::dispatch` drops the
work; the only fallback is a second, hand-written C++ copy of the kernel,
which drifts from the first the day someone edits one of them. And even where
a device exists, a round trip through a command buffer is the wrong shape for
a 64-sample block due back in a millisecond.

**Correctness.** Every numeric check of the codegen needs a device.
`GPUCodegenTests` compares emitted strings and, where `eacp-spirv` is built,
compiles the GLSL through glslang; that proves the text is well formed, not
that it computes the right thing. The numeric suites are in `GPUTests`, and
every case there self-skips without a device — so of the three Linux CI lanes,
the two without lavapipe (GCC and Clang) prove only that GLSL compiles. An
executor that runs the graph itself is a second oracle that needs nothing,
runs on every lane, and, where a device exists, can be cross-checked against
the GPU's readback of the very same `ComputeProgram`. Three readings of one
graph — the emitter's, the interpreter's and the test's inline reference —
also localise a disagreement: two against one names the culprit.

This is not a replacement for the GPU on throughput, and it is not a general
C++ back end for the EDSL. It is the kernel the app already wrote, run where
the GPU cannot or should not run it, at a cost that is a small multiple of the
hand-written loop.

## 1. What is there and what is missing

**The graph** (`Codegen/ShaderGraph.{h,cpp}`, 818 + 994 lines). A kernel
records into a fully materialised, index-based expression DAG plus a
structured statement tree, and nothing about it is tied to a device. A node is
`Expr {kind, type, index, value, op, text, args}`: 25 `ExprKind`s, of which
`Input` and `Varying` are render-only and `Sample`/`Fetch` read textures; the
other 21 are what a compute body is made of (`Constant`, `Uniform`,
`Construct`, `Swizzle`, `Call`, `Unary`, `Binary`, `Compare`, `Select`,
`VarRead`, `Mul`, `ThreadId`, `BufferRead`, `BufferVectorRead`, `AtomicLoad`,
`ArrayRead`, `LocalId`, `GroupId`, `GridExtent`, `SharedRead`,
`SimdGroupIndex`). A Float constant is in `value`; a UInt, Int or Bool one is
in `index`, and `addUIntConstant` stores `(int) value`, so a UInt above
`INT_MAX` reads back negative and has to be reinterpreted as `uint32_t`.
`Binary` carries its operator in the `op` char except for the two shifts,
which are in `text`; `Compare` carries it in `text`; `Call` names the builtin
in MSL spelling — the math set (`sin` … `tanh`, `atan2`, `rsqrt`, `fract`,
`mix`, `step`, `smoothstep`, `clamp`, `pow`, `dot`, `cross`, `length`,
`normalize`, `distance`, `reflect`, `refract`, `faceforward`, `determinant`,
`transpose`, `all`, `any`, `sign`, `trunc`, `round`, `log10`, …), a
constructor-style conversion under the target type's name (`float`, `uint2`),
a bitcast as `as_type<…>`, and the seventeen `eacp*` helpers. Mixed widths are
left to the target language: `Float3 * Float` is one `Binary` with a
one-component operand, and `clamp(v, 0.f, 1.f)` one `Call` with two
(`ShaderValue.h`, "broadcast across vectors"). The 17 `StatementKind`s are
`Declare`, `Assign`, `If`, `Loop`, `Break`, `Continue`, `Store`,
`VectorStore`, `TextureStore`, `SharedStore`, `Barrier`, `GroupReduce`,
`SimdMatrixFill`, `SimdMatrixLoad`, `SimdMatrixStore`, `SimdMatrixMultiplyAdd`
and `AtomicAdd`, held in `Block`s by index, with `rootBlock = 0` the body. The
side tables are `storageBuffers()` with `storageElementType(slot)`,
`uniforms()`, `variables()`, `sharedArrays()`, `arrays()`,
`groupReductionTypes()`, `simdMatrixCount()`/`simdMatrixElement()`,
`dispatchRank()`, `threadGroupShape()`, `usesBarrier()`, `usesLocalId()`,
`usesGroupId()` and `usesSimdGroups()`; `simdGroupWidth = 32` and
`simdMatrixSize = 8` are constants beside them.

Two properties of the graph decide how it has to be executed. First, sharing:
constants, pure binaries and reads of `BufferAccess::Read` slots are
hash-consed (`findShared`, keyed by `constantKeyFor`/`binaryKeyFor`/
`readKeyFor`), so one node can feed statements far apart. Purity is
`purityOf`: not one of the kinds in `dependsOnMutableState` (`VarRead`,
`BufferRead`, `BufferVectorRead`, `AtomicLoad`, `Sample`, `Fetch`,
`SharedRead`) unless `readsImmutableStorage`, and every argument pure — but
`isPure(int)` is private. An impure node's value is the one it has *where the
statement using it runs*, which the emitter enforces with `readsStale` (a
`VarRead` of a variable just written, a read of a buffer just stored to, a
`SharedRead` after shared memory moved). Second, record stores:
`addRecordStore` lays down one `Store` per component with `record` naming the
record's node and `recordComponentsLeft` counting down, and every component
takes the value the record had before the first of them ran — the emitter's
`holdTheRecord` binds the record once for that reason
(`InPlaceRecordTests` pins it).

**The emitter** (`Codegen/ShaderEmitter.cpp`, 3945 lines). All three dialects
are one walker branching on a private `enum class Backend {Metal, DirectX,
Vulkan}`; `callName` renames the MSL builtins per dialect, `ExprPrinter`
prints a node, `countUses`/`orderLocals` hoist a node used more than once
into a local, `StageEmitter` walks the blocks and drops a local that
`readsStale` says a statement invalidated, and `emitCompute` writes the entry
point with the `boundsGuard(rank)` early return — omitted when
`usesBarrier()`, since a return ahead of a barrier the rest of the group waits
at is undefined; such a kernel bounds its own stores against `GridExtent`. The
helpers exist only as source text in the `shaderHelpers` table (18 entries:
the 17 `eacp*` — `eacpErf`, `eacpErfc`, `eacpSaturatingTanh`, the half,
bf16, int8 and int4 pack/unpack/read family — plus a GLSL-only `log10`). What
the two fallback dialects do in place of Metal's SIMD-group intrinsics is
already a precise statement of lane semantics: `groupReduction` stages every
lane's contribution in scratch and folds by halving strides
(`reductionStride(width)`), narrowed to 32-lane blocks for `ReductionScope::
Simd`; a SIMD-group matrix is spread over the lanes of its SIMD group, lane
`l` holding elements `2l` and `2l + 1` of the row-major 8x8
(`simdMatrixPreamble`, the comment at line 1140).

**The kernel type** (`Codegen/ComputeProgram.h`, 885 lines). `ComputeProgram`
owns a `ShaderBuilder`, the `GeneratedShader`, the uniform bytes, and
`std::optional<ShaderLibrary>`/`std::optional<ComputePipeline>`. `compile()`
runs the `ShaderBuildVisitor` walk, `define()` and `builder.build()` —
device-free already — and `graph()` hands the recorded graph back afterwards,
which `Tests/GPU/ProgramGraphTests.cpp` already uses as a third consumer. What
ties the type to `eacp-gpu` is linking, not including: `ShaderLibrary` and
`ComputePipeline` are `Pimpl` types whose destructors live in `eacp-gpu`, so
any kernel struct needs `eacp-gpu` at link time. The includes are harmless on
their own — `Tests/GPU/CodegenCommon.h` already pulls `Frame/ComputePass.h`
and `Frame/RenderPass.h` into the device-free `GPUCodegenTests`. The same holds
one level down: `Uniform<T>`, `ShaderVisitor`, `ShaderBuildVisitor`,
`ShaderUploadVisitor`, `CpuValueOf`/`ShaderValueOf` and the `EACP_SHADER`
macro engine all sit in `ShaderProgram.h` (1560 lines) beside the render
`ShaderProgram`, which includes `Device.h`, `RenderPass.h` and
`StreamingBuffers.h`. `grep -rl ComputeProgram` finds 54 files; 158 structs
derive from it directly (147 in `Tests`, 6 in `Apps`, 5 in `Lib`, one of those
the doc comment in `ComputeProgram.h`) and three more through `GPUWidgets`'
`PathIndexedKernel` (`BinKernel`, `CoverageKernel`, `BackdropScanKernel` in
`Path/BackdropKernels.h`).

**Buffers and uniforms.** A storage buffer is a flat array of `float` or
`uint32_t` (`storageElementType`); `read2/3/4` and `write2/3/4` are tight
N-stride records of consecutive elements, and there are no struct layouts.
`Uniform<InputBuffer>` and its four siblings hold a `BufferRange {const
Buffer*, offset, bytes}`, which has no CPU pointer: `Buffer` offers `read`
and `update` copies and, on Metal only, adoption of caller memory through
`ExternalMemory` (`canAdoptMemory`), but no persistent mapping. A value
uniform is CPU-visible as `Uniform<T>::value` (column-major `Array<float, 16>`
for a `Float4x4`), and `ShaderVisitor::onUniform(name, type, handle, data)`
hands out its type, its handle (whose `node` is the graph's `Uniform` node,
`index` the slot) and a pointer to the value. `ShaderUploadVisitor` packs
those into MSL layout (`uniformAlignment`, `uniformSlotStride`) and
`packWithExtents` appends the grid extents; a CPU reader wants the typed
values, not the packed block.

**Dispatch** (`Frame/ComputePass.h`). `dispatch(count)`, `(width, height)`,
`(width, height, depth)` in threads, rounded up to groups of the pipeline's
own shape or the stock one — `threadGroupWidth = 64`, `threadGroupSize2D = 8`
squared, `threadGroupSize3D = 4` cubed; `maxBufferSlots = 8`.
`dispatchIndirect(Program&, const Buffer&, int guardCount, offset)` is 1D
only: it reads `DispatchArguments {groupsX, groupsY, groupsZ}` — group counts,
not thread counts — and takes the guard's extent as a *capacity* the caller
passes.

**SIMD and threads.** `eacp-simd` (`Lib/eacp/SIMD/`) has `simd::F32` in
`Vector.h` with `zero`, `broadcast`, `load`, `loadBroadcast`, `store`, `+`,
`-`, `*`, `min`, `max`, `fma` and `reduceAdd` — no masks, no integers, no
compare, divide, sqrt or transcendentals — and `Tu/ArrayOps.cpp`, plain loops
the compiler auto-vectorises. The module is built with
`eacp_force_optimization` (`CMake/TargetSetup.cmake`: `-O3
-ffp-contract=off` in every configuration, `/clang:` forms under clang-cl,
`/O2` under cl), which on MSVC strips `/RTC1` and `/Od` from the
*directory-scoped* Debug flags — so a force-optimised target wants a directory
of its own. `Core/Threads/` has `Timer`, `Async`, `callAfter`, the event loop
and `TaskSemaphore`; there is no thread pool and no `parallelFor` anywhere.

**Tests** (`Tests/GPU/CMakeLists.txt`). `GPUCodegenTests` (four sources,
`TARGETS eacp-gpu-codegen`) is built before the `if (NOT EACP_HAS_GPU)
return ()`; `GPUTests` (`NO_MAIN`, ~70 sources, `TARGETS eacp-sprites
eacp-gpu`) after it. Tolerances are per suite (`IntrinsicTests`' `near(gpu,
reference, tolerance)`, `1.0e-6 + 1.0e-5 * |reference|` for the transcendental
set). Metal compiles with its default math mode — fast math; nothing in
`ShaderLibrary-Apple.mm` sets one.

**What is missing.** No interpreter or software path of any kind; a kernel
type that cannot be linked without `eacp-gpu`; no way to bind CPU memory to a
kernel's buffer slots; no C++ twin of any `eacp*` helper (each test that needs
a reference writes its own); no masked, integer or transcendental lane
arithmetic; no pool to parallelise over.

## 2. Decisions

**D1 — An interpreter over `ShaderGraph`.** The executor walks the graph the
kernel already recorded, decoded once into a flat op list. Rejected:

- *`emitCpp` plus a build-time compile.* A kernel is a runtime object built in
  a constructor, often parameterised by what the constructor was handed (a
  group shape, a tile size, a mode), and the build has no step that could run
  a constructor and compile what it emitted. A fourth dialect in
  `ShaderEmitter.cpp` would be easy; getting its output into the binary is
  not.
- *A JIT.* No LLVM, no assembler, nothing of the kind in the tree, and code
  generation at runtime is exactly what a realtime thread and an iOS build
  cannot have.
- *Re-templating the front end* so a kernel's body compiles to C++ directly.
  The handles in `ShaderValue.h` (4027 lines) are runtime `{graph, node}`
  pairs by design, and so is everything above them; the rewrite would be the
  EDSL twice.

**D2 — One batch is one threadgroup, all lanes in lockstep under an execution
mask.** Every node's value is a lane array, SoA: `components × lanes` 32-bit
words, component-major (all lanes of `.x`, then all of `.y`), so each
component operation is one dense loop over lanes. Floats are `float`,
`UInt`/`Int` are both stored as `uint32_t`, `Bool` is an all-ones/all-zeros
mask word so `Select` and masking are bitwise; a matrix is its columns'
components in MSL's column-major order. Only `+`, `-`, `*`, the bitwise
operators and `<<` are sign-blind, and those run on the unsigned lanes, so
C++ wraparound stays defined. Every operation whose result depends on the sign
reads the lanes as `int32_t` when the node's operand type satisfies
`isSignedInteger` (`ShaderTypes.h`): `/` and `%` (truncating, C++ semantics,
with D7's rows for zero and `INT_MIN / -1`); `Compare` `<`, `<=`, `>`, `>=`;
`>>`, arithmetic for `Int` and logical for `UInt` — the emitter prints `>>`
unchanged on both, and MSL, HLSL and GLSL all sign-extend a signed right
shift, which C++20 now guarantees too; `abs`, `sign`, `min`, `max`, `clamp`;
`Unary -`; `toFloat` of an `Int` (through `int32_t`); float → `Int`
(saturating, D7); and `toInt`/`toUInt` between the two integer types, a
bit-preserving reinterpretation as on the GPU. Storage stays `uint32_t`
throughout; only those operations cast. The batch width is the group's
`threadCount()`: 64 for the stock 1D shape, 8x8 and 4x4x4 for the other
two, whatever `ComputeProgram({256})` or `({16, 16})` asked for otherwise; lane `l` is the flat local index, x fastest,
which is what the emitter's `lane` is. Consequences, each one a place where
lockstep makes a GPU mechanism trivial:

- `Barrier` is a no-op: statement order already is the barrier.
- A `SharedArray` is one array per group; `SharedRead`/`SharedStore` index it
  per lane.
- `GroupReduce` is a horizontal fold over the batch; `ReductionScope::Simd`
  folds each 32-lane block (`simdGroupWidth`) separately. The fold follows the
  fallback's halving tree exactly, so a float sum rounds as it does on D3D12
  and Vulkan.
- `SimdGroupIndex` is `lane / 32`.
- A SIMD-group fragment is held whole, as a dense 8x8 per SIMD group, not
  spread over lanes: the lane distribution of the emitter's emulation is
  unobservable (a fill takes only a constant — `ShaderBuilder::simdMatrix
  (float)` — and fragments leave only through `SimdMatrixStore`), so
  `SimdMatrixMultiplyAdd` is an 8x8 matrix product per SIMD group. A
  load/store whose offset or stride differs across the lanes of one SIMD group
  (Metal requires them uniform) takes the first active lane's. *As built
  (stage 3):* fragment `f` of SIMD group `g` is 64 row-major words at
  `fragmentOffset + (f·simdGroups + g)·64`, `simdGroups = ceil(lanes/32)`,
  laid out after the shared arrays and zeroed with them at group start. Each
  statement acts on a SIMD group's whole fragment when any of its lanes is
  active, taking the first active lane's fill value, offset and stride, and is
  skipped otherwise. Element `(r, c)` is at `offset + r·stride + c` in wrapping
  `uint32`; out of range reads 0 and a store there is dropped. The product is
  D = C + Σ_k L[r][k]·R[k][c], the accumulator first and k ascending, unfused,
  into a stack copy, so the accumulator may be an operand. A packed half or
  bf16 load counts in sixteen-bit elements as the fallback does: element `i`
  is half `i % 2` of word `i / 2`, widened as it loads through `readHalf`/
  `readBFloat16` (`Helpers.h`), so every fragment holds floats; a word out of
  range reads 0. A group whose `threadCount()` is not a multiple of 32 with
  any `SimdMatrix*` statement is refused at plan time, naming the count: Metal
  needs whole SIMD groups, and the fallback's `sgmScratch[threads / 32 * 128]`
  is too small for a partial one (48 threads get one SIMD group's scratch,
  16 get none). The emitter only asserts it (`emitCompute`, Debug only), for
  any kernel that `usesSimdGroups()`; `ShaderBuilder` and `ComputeKernel`
  refuse nothing.
- `If` computes `then = active & cond`, `else = active & ~cond`, and skips a
  body whose mask is empty. `Loop` runs while any lane is live: the condition
  is re-evaluated for the loop's lanes each iteration and clears the ones it
  fails, `Break` clears lanes from the loop mask, `Continue` from the
  iteration mask until the body's end. The mask stack's depth is the graph's
  deepest `If`/`Loop` nesting, known at plan time.
- The bounds guard follows `emitCompute`: in a kernel that does not
  `usesBarrier()`, lanes whose global id is at or past the extent start
  inactive; in one that does, every lane of a partial group runs, as it does
  on the GPU, and the kernel's own `GridExtent` checks bound its stores.

Arithmetic runs on *every* lane, masked or not, and only side effects are
masked: a store, an atomic, a shared store, and an assignment (a blend of new
and old under the mask). That keeps every arithmetic loop branch-free and
vectorisable, and it is safe because D7 makes every operation total — a read
at a garbage index reads 0, a division by zero yields 0. Rejected: *one thread
at a time*, which gives up SIMD entirely and needs a coroutine or a fiber per
thread to suspend at each barrier; *phase-splitting at barriers* (run every
thread to the barrier, then every thread past it), which breaks as soon as a
barrier sits inside a loop — `GroupReductionTests`' `RowSumKernel` folds with
`groupSum` inside a recorded `loop`, and `SimdMatrixTests` stages tiles with
`barrier()` inside one. (`ScanBlockKernel`'s barriers are not: its doubling
loop is a C++ `for`, unrolled as the graph is recorded, for exactly this
reason.)

*As built (stage 2):* `LocalId` has no scratch of its own — its node points at
the workspace's local-coordinate rows; `GroupId` is filled once per group and
`SimdGroupIndex` once per workspace. The shared arrays sit in one block, each
one run of words per group, element-major (`storage + e * components + c`),
zeroed at group start after the guard; shared indices are unsigned, so a
negative one is out of range. Shared memory is capped only by the existing
`INT32_MAX`-word scratch limit (`SharedMemoryTests`' 1M-float array is 4 MB
here). `GroupReduce` stages into one reduction row, allocated only when the
kernel reduces, and folds it with the fallback's bounded halving tree; `Simd`
blocks are `min(threads, 32)` wide and a partial last block folds only the
lanes it has. Only `Float`, `UInt` and `Int` fold — anything else is refused —
and D7's identity for an inactive lane is `-0.0f` for a float sum, `-inf`/
`+inf` for a float max/min (which fold through `fmax`/`fmin`), 0 for an
integer sum, 0 / `UINT32_MAX` for a uint max/min and `INT32_MIN`/`INT32_MAX`
for an int max/min. With a barrier dropping the guard, the lanes of a last group past the
extent run, read 0, fold, and store wherever the output has room;
`GPUCodegenTests` pins both sides of that.

**D3 — Plan once, execute without allocating.** Construction is the plan: it
decodes the graph into a flat op list — each `Call` name, `Compare` text and
shift text resolved to an enum, each operand to a scratch slot, every `Expr`
string left behind — rejects what the executor does not support (D11) with
`isValid()` false and a reason, and allocates everything execution touches:
per-node lane scratch, variable storage, shared arrays, array constants,
SIMD fragments, the mask stack, the epoch stamps, the per-slot uniform words
and the binding table. Logging happens here and only here. Execution takes
spans and uniform values, never allocates, locks, logs or makes a syscall, and
runs on the calling thread. The split is two objects, not one: an immutable
`Plan` (the decoded program and the layout of the scratch) and a mutable
`Workspace` (the scratch itself), so stage 4's parallel groups are one
workspace per worker over one plan; v1's `Executor` bundles one of each. In v1
every node gets its own scratch slot: 500 nodes × 64 lanes × 4 components × 4
bytes is 512 KB, acceptable for a kernel of that size. Reusing slots by
liveness is stage 4's. *As built:* `Plan`/`Workspace` with `Executor`
bundling one of each, as planned, plus an internal `Interpreter.h` — the
`Context`/`SlotView`/mask-frame seam between the source files. `laneStride` is
the lane count rounded up to 16; `footprintBytes()` for the one-line gain
kernel at 64 lanes is 2432 bytes (4 nodes × 64 words, the mask,
local-coordinate and real-lane rows, 16 uniform words and 64 bytes of
alignment slack). A dispatch of the wrong rank returns false rather than
asserting. *As built (stage 3):* the `eacp*` calls decode to one
`Op::Helper`, appended to the `Op` enum, with a `HelperFunction` in
`Node::sub` (`decodeHelper` in `Plan.cpp`, run by `builtinHelper` in
`Builtins.cpp`). The 3 componentwise helpers (`eacpErf`, `eacpErfc`,
`eacpSaturatingTanh`) take any float width, the scalar applied per
component; the 14 packed ones are held to their one signature, argument and
result types checked; an unknown `eacp*` name is refused by name.
`Evaluate.cpp` is unchanged — a helper is one more `Call`.

**D4 — Statements in order, expressions by epoch.** The executor walks the
blocks; each statement evaluates the expression trees it names recursively,
stamping each node with the current epoch. A pure node already stamped this
epoch is not recomputed; an impure one always is. The epoch advances per
statement execution, so a loop condition re-read each iteration and a
`VarRead` after an `Assign` are both fresh — the staleness rule `readsStale`
enforces in the emitter, reached here by never carrying an impure value
across a statement at all. A statement evaluates everything for all lanes
first and commits afterwards (stores in ascending lane order, so the last lane
wins a conflicting store — a race on the GPU, deterministic here). A record
store pins its `record` node at the first component statement and keeps it
through `recordComponentsLeft == 0`, as `holdTheRecord` does. Array constants
are evaluated once per group, before the body, as the emitter declares them.
`ShaderGraph::isPure` becomes public for the plan; nothing else in the graph
changes. Hoisting a pure node across statements — what `countUses`/
`orderLocals` do — is a stage-4 optimisation keyed on the same purity bit,
and so is evaluating lane-invariant nodes (a constant, a uniform, a
`GridExtent`, a `GroupId` and arithmetic over only those) once per group or
per dispatch instead of per lane; v1 splats constants and uniforms once per
dispatch and nothing more. *As built:* no epochs and no recursion. The plan
computes a post-order schedule per statement — a flat, deduplicated node list
for the value and index of a store, an `If`'s condition, a `Loop`'s condition
per evaluation — and a statement runs its list as a flat loop: fresh every
statement, shared within one, the same semantics. The record pin is plan-time
too: the first component's schedule includes the record node, the following
components' treat it as a leaf. `VarRead` has no scratch of its own and reads
the variable's storage, which is safe because a statement commits after it
evaluates. Array constants are evaluated at group start, after the guard, in
slot order, into array storage. Stage 1 did not need `isPure`; it stays public
for stage 4's hoisting.

**D5 — Auto-vectorised lane loops first, intrinsics only where measured.** The
lane operations are a small internal layer (`CpuCompute/Lanes.h`): templated
loops over `const float*`/`uint32_t*` of the batch width, written like
`Tu/ArrayOps.cpp`, in a target built with `eacp_force_optimization` so it is
`-O3 -ffp-contract=off` in every configuration, Debug included — an audio
thread cannot run an interpreter at `-O0` — plus `-fno-math-errno` so `sqrt`
and friends inline (IEEE semantics unchanged; nothing reads `errno`). Not an
extension of `simd::F32` in v1: `F32` has no masks, integers, compares,
division or transcendentals, and growing it is a design question for the
image kernels that own it, not a side effect of this plan. Transcendentals are
`std::` calls per lane in v1. Hand intrinsics come in stage 4 and only where a
benchmark says so — the likely three are the gather under a dynamic
`BufferRead`, masked select/blend, and a vector `sin`/`exp` — and if they are
general they fold into `eacp-simd` then.

**D6 — The device-free seam: `ComputeKernel` under `ComputeProgram`.** A new
base class in `eacp-gpu-codegen`, `Codegen/ComputeKernel.h`, takes everything
of `ComputeProgram` that is not a device: the `ShaderBuilder`, the
`GeneratedShader`, `compile()`, `graph()`, `source()`, the pure virtuals
`reflectMembers` and `define`, the whole `define()` vocabulary (`threadId`
through `write`), `packedUniforms`/`packWithExtents`, `dispatchRank()`,
`groupShape()`, `threadgroupMemoryBytes()`, `uniformByteSize()` and the
`groupWidth`/`groupSize2D`/`groupSize3D`/`simdWidth`/`simdMatrixWidth`
constants (it includes `Frame/ComputePass.h` for them, header-only, as
`CodegenCommon.h` already does). `ComputeProgram : public ComputeKernel` keeps
`prepare(Device&)`, `prepare()`, `pipeline()`, `isValid()`,
`bindResources(ComputePass&)`, `fitsThreadgroupMemory`,
`fitsPackedSimdMatrix`, the three `report*` functions,
`buildRefusedPipeline`, `ComputeBindVisitor` and the two optionals. The member
machinery moves the same way: `Uniform<T>`, `ShaderVisitor`,
`ShaderBuildVisitor`, `ShaderUploadVisitor`, `CpuValueOf`, `ShaderValueOf`,
`memberOffset` and the `EACP_SHADER`/`EACP_SHADER_VALUE` macros go from
`ShaderProgram.h` into `Codegen/ShaderMembers.h`, which `ShaderProgram.h`
includes, so every existing include keeps compiling (`Texture` is only ever a
pointer there and is forward-declared). *As built:* `ShaderMembers.h` also
carries `ShaderValueIs`; `toVertexFormat`, `VertexFormatOf`,
`expectedAttributeBytes`, `ShaderTextureBindVisitor`,
`ShaderBufferBindVisitor` and `ShaderGraphVisitor` stay in `ShaderProgram.h`,
since they need `RenderPass` or vertex input; no `Texture` forward declaration
was needed — `ShaderTypes.h` already includes `Texture/Texture.h` — and
`ShaderMembers.h` includes `Buffer/Buffer.h`, since `BufferRange` is held by
value. `packWithExtents` stays private on `ComputeKernel`, and
`ComputeProgram::prepare()` reads `source()` rather than the now-private
`generated`. Every one of the 158 kernel structs
keeps deriving from `ComputeProgram` and is untouched; a kernel meant to link
without `eacp-gpu` — `CpuComputeTests`', a CPU-only product's — derives from
`ComputeKernel` and is otherwise written identically. `ComputePass`'s
`dispatch(Program&, …)` templates are unaffected. *As built (stage 1):*
`ComputeKernel` gained a public `visitMembers(ShaderVisitor&)` forwarding to
the protected `reflectMembers`, which is what the executor walks. It has no
`array()` — only `ShaderProgram` and `ShaderBuilder` do — so the array-constant
tests record on a bare builder.

Rejected: *making `ComputeProgram` itself link-inert* by holding the GPU state
behind a type-erased holder created in `prepare()`. It works, and it would let
a device-free binary instantiate an unchanged kernel, but it hides a link
dependency behind a trick every future member would have to respect, where the
split states it in the type. *Moving the GPU half out into a wrapper*
(`GpuKernel<T>`) instead of a derived class: it changes every `prepare()` and
`pass.dispatch(kernel, …)` call site in the tree.

Binding, on the CPU side, is a separate object, not a second field on the
member: `CpuCompute::Bindings`, a fixed table indexed by slot, filled through
typed setters that take the kernel's own member so the slot comes from its
handle — `bindings.set(kernel.input, std::span<const float>)`, `set(kernel.
output, std::span<float>)`, and the `uint32_t` forms for `UIntInputBuffer`,
`UIntOutputBuffer` and `AtomicBuffer`. The spans are the caller's memory; the
executor checks each span's element type against `storageElementType(slot)`
and its access against `storageBuffers()[slot]` at bind time, which is cheap
and allocation-free. Rejected: *a visitor mapping each member's `BufferRange`
to a span through a user callback* — it presupposes a `GPU::Buffer`, which a
CPU-only caller cannot make without a device, and `Buffer` has no persistent
CPU pointer to map to anyway; *adding a span beside the `BufferRange` inside
`Uniform<InputBuffer>`* — it makes one kernel object hold two bindings that
have to be kept consistent, and ties the CPU binding's lifetime to the
kernel's. The separate table also lets one kernel run on the GPU over its
buffers and on the CPU over plain arrays at once, which is exactly what the
cross-check does.

Uniform values are read through a fourth visitor beside `ShaderBuildVisitor`,
`ShaderUploadVisitor` and `ComputeBindVisitor`: `CpuUniformVisitor`, whose
`onUniform` copies `byteSize(type)` bytes from `data` into the plan's
preallocated words for the slot `graph.expr(handle.node).index` — typed
values, no MSL packing. `reflectMembers` is a virtual call on a stack visitor
with no allocation, so it runs inside `execute` on every dispatch and picks up
`kernel.gain = …` set a moment earlier on the same thread. The implicit
`GridExtent` values come from the dispatch call, as they do on the GPU. A bare
graph (a `GPUCodegenTests` case recorded on a `ShaderBuilder`) is executed
through the same plan with the uniform words set by slot.

**D7 — Numerics: IEEE on the CPU, tolerances in the tests, every undefined
case defined.** The CPU is not bit-exact with the GPU and does not try to be:
Metal compiles with fast math, the others with whatever their compilers
contract. The executor is IEEE single precision with no contraction; the
cross-checks use each suite's existing tolerances. Every `eacp*` helper gets a
C++ implementation (`CpuCompute/Helpers.{h,cpp}`, public) that is the single
source of truth for the executor and for the tests' references, replacing the
per-test copies as those tests are touched. `log10` is `std::log10`. What the
GPU leaves undefined, the CPU defines, and the README documents:

| Case | GPU | CPU |
| --- | --- | --- |
| buffer read out of bounds (or unbound slot) | Metal undefined; D3D12/Vulkan robust access reads 0 | reads 0 |
| buffer store / atomic out of bounds | Metal undefined; D3D12/Vulkan drop it | dropped; the atomic yields 0 |
| shared or constant array index out of bounds | undefined everywhere | reads 0, store dropped |
| integer `/` or `%` by zero | Metal/Vulkan undefined; D3D gives all-ones | 0 |
| `INT_MIN / -1` | undefined | `INT_MIN` |
| `INT_MIN % -1` (*as built*) | undefined | 0 |
| `toInt`/`toFloat` of a `Bool` (*as built*) | 1 / 1.0 for true | 1 and 1.0f for true, 0 for false |
| `fract` (*as built*) | Metal: `min(x - floor(x), 0x1.fffffep-1f)` | Metal's formula |
| shift by ≥ 32 | amount masked on the hardware | amount `& 31` |
| float → int/uint out of range, NaN | saturates on the hardware | saturates; NaN → 0 (C++ leaves this UB, so it is spelled out) |
| `Barrier` under divergent control flow | undefined | no-op |
| `GroupReduce` under divergence | undefined | inactive lanes contribute the fold's identity; only active lanes receive the result |
| SIMD-group matrix op under divergence (*as built*) | undefined | acts on the whole fragment if any lane of the SIMD group is active, with the first active lane's operands; skipped otherwise |
| integer overflow | mod 2^32 | mod 2^32; `+`, `-`, `*` run on the unsigned lanes, the sign-sensitive operations of D2 on `int32_t` |
| threadgroup memory at group start, and SIMD-group fragments (*as built*) | uninitialised | zero-filled |

Reads returning 0 follow the two backends that define the case, which is also
what makes D2's "compute on every lane" safe. A UInt constant's `index` is
read back as `uint32_t`. *As built:* `round` is `std::round`; `round` at an
exact half is away from zero (`std::round`, Metal's rule); D3D12 and Vulkan
round to even, so a cross-checked kernel must avoid exact halves or use
`floor(x + 0.5)`. `sign(NaN)` is
0; a partly out-of-range `VectorStore` or `read2/3/4` is checked per element.
A slot the kernel reads or writes that is never bound refuses the dispatch
(false); a slot bound to an empty span runs, reading 0 and dropping stores.
`count <= 0` returns true and does nothing. A store to an `Atomic` slot is
accepted as a plain store.

*As built (stage 3):* `Helpers.{h,cpp}` is public, as planned, with two
naming deviations: `errorFunction`/`complementaryErrorFunction`, because an
unqualified `erf` under a using-directive is ambiguous with `::erf(float)`,
and `widenHalf`/`narrowToHalf`, because `GPU::halfToFloat` already exists in
`PackedVertex.h`; the rest are the shader names without `eacp`
(`packHalf2`, `readBFloat16`, `unpackInt4x4`, …). `packHalf2` follows Metal:
nearest-even including subnormal ties, overflow to infinity — unlike
`GPU::halfFromFloat` in `PackedVertex.cpp`, which rounds subnormal ties away
from zero through `lround`. A NaN narrows to `sign | 0x7e00 | (mantissa >>
13)`. Parity and byte-index shifts are `& 31`, as the hardware masks them.
`errorFunction`, `complementaryErrorFunction` and `saturatingTanh` are the
shader's operations in float with no fused multiply-adds, so CPU against GPU
is within tolerance, not identical. D3D's `f32tof16` rounds toward zero and
saturates and GLSL's `packHalf2x16` rounds as the driver does; the twin is
Metal's.

**D8 — Serial over groups in v1, atomics ready for more.** Groups run in
order, x fastest, on the caller's thread. `AtomicAdd` and `AtomicLoad` go
through `std::atomic_ref<uint32_t>` with relaxed order from the first commit
(`static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free)`), lanes
in ascending order, each lane getting the pre-add value, so stage 4's parallel
groups change nothing in the executor. Core has no pool and this plan does not
add one: stage 4 exposes `dispatchGroups(first, count, Workspace&)` and lets
the caller's own threads — an audio host's worker pool, a render thread —
run disjoint ranges. *As built (stage 2):* as planned; the pre-add value lands
in the variable of active lanes only, and an atomic op on a non-`Atomic` slot
is refused at plan time. The ordering is observable and the tests use it:
each statement finishes across the whole group before the next, so every
lane's `load` after an add returns its group's final count exactly
(`AtomicCodegenTests`' run case) — which no GPU guarantees, so the
`GPUTests` ticket cases check each backend for a permutation of `0..n-1` and
never compare tickets across backends.

**D9 — `Lib/eacp/GPU/CpuCompute/`, target `eacp-cpu-compute`, namespace
`eacp::GPU::CpuCompute`.** The `Spirv` precedent exactly: a subdirectory of
`GPU` with its own `CMakeLists.txt` (`eacp-spirv`, namespace
`eacp::GPU::Spirv`), which is also what `eacp_force_optimization`'s
directory-scoped flag edit requires. It is entered from
`Lib/eacp/GPU/CMakeLists.txt` right after `eacp-gpu-codegen` and before the
`if (NOT EACP_HAS_GPU) return ()`, links `eacp-gpu-codegen` PUBLIC and nothing
device-related, and so builds on every platform, a Linux box with no driver
and `-DEACP_BUILD_GRAPHICS=OFF` included. Tests:

- `CpuComputeTests` — a device-free NanoTest binary in a new `Tests/CpuCompute/`
  (`TARGETS eacp-cpu-compute`), added in `Tests/CMakeLists.txt` beside
  `add_subdirectory(GPU)` under the same `NOT IOS`. Its own directory rather
  than beside `GPUCodegenTests` in `Tests/GPU/CMakeLists.txt`, because the
  benchmark that lives with it (stage 4) is force-optimised the way
  `Tests/SIMD/CMakeLists.txt` does `SimdBench`, and on MSVC that edits the
  directory's Debug flags for `GPUCodegenTests` and `GPUTests` too. It checks
  the executor's own semantics (masks, loops, record stores, every row of D7)
  and whole kernels against inline references.
- `GPUCodegenTests` gains `eacp-cpu-compute` in its `TARGETS` (stage 2), so
  the graphs its string tests already build are also run and checked for
  value — the atomic, reduction and SIMD-matrix codegen cases first.
- `GPUTests` gains `eacp-cpu-compute` in its `TARGETS` and a
  `Tests/GPU/CpuCrossCheck.h` beside `Common.h`: a helper that runs a
  `ComputeProgram` on the CPU over plain arrays and, where a device exists,
  on the GPU over buffers holding the same data, and compares both against
  the case's reference. The CPU half never self-skips, so the two Linux lanes
  without a driver run every kernel in the listed suites numerically.

*As built:* `CpuComputeTests` needs `-ffp-contract=off` on the test target
too — Clang contracts the multiply-adds in the tests' inline C++ references
even in Debug, and the exact checks against the executor then fail. `GPUTests`
does not: its exact float cases have nothing contractable. There is no
environment switch that disables the Metal device, so "never self-skips" was
proven by forcing the GPU half off temporarily (478/478) and by corrupting the
CPU result (exactly the 36 cross-check cases failed, each naming `cpu`).
*As built (stage 2):* `GPUCodegenTests`' `TARGETS eacp-gpu-codegen
eacp-cpu-compute` is unguarded — both targets sit above the `EACP_HAS_GPU`
return — so its `…/runs` cases run on every lane, driverless Linux included.
Each suite records one graph for its emit and run cases, in a small struct
holding a `ShaderBuilder` and its buffer handles (neither copyable nor
movable, since the handles point into the builder); a bare graph runs through
`Executor {graph}`. The reduction references are a local copy of the fallback
tree over non-dyadic inputs, compared with `==`, copied rather than shared
with `GroupTests.cpp` because the two binaries share no header.
`GPUWidgetsTests` links `eacp-cpu-compute` too. Multi-kernel handoffs
(the indirect pipelines, the histogram, `BinKernel` → `CopyUIntKernel`) are
bound by hand over one host array on the CPU and compared with the GPU's
buffer; a case with two float tolerances compares the backends at the looser.

**D10 — Dispatch mirrors `ComputePass`.** `Executor::dispatch(bindings,
count)`, `(bindings, width, height)` and `(bindings, width, height, depth)`
take threads, round up to groups of the kernel's `groupShape()` and assert
the rank as `packedUniforms` does. `dispatchIndirect(bindings, std::span<const
std::uint32_t> arguments, int guardCount, int offsetInElements = 0)` mirrors
`ComputePass::dispatchIndirect(Program&, …)`: 1D only, `DispatchArguments`
group counts read from the span, `guardCount` the capacity the guard reads; an
offset past the span's end dispatches nothing, as on the GPU. The 1D id comes
from the group's x alone, so groups in y and z repeat the x range, as a 1D
kernel's `gid` does on every backend. Every dispatch returns whether it ran
(false for an invalid plan or a slot left unbound that the kernel reads —
checked, not logged). *As built:* unchanged, except that a wrong rank returns
false rather than asserting. *As built (stage 2):* `dispatchIndirect` as
planned. An offset that leaves no whole `DispatchArguments`, a negative
offset or a zero group count runs nothing and returns true, like
`dispatch(0)`; false still means an invalid plan, a wrong rank or a bad slot.
A negative `guardCount` is 0 — and a kernel with barriers, having no guard,
still runs every lane then, as on the GPU. `GroupId.x` in y and z is the x
group.

**D11 — Out of scope for v1.** Textures: `Sample`, `Fetch`, `TextureStore`
and `WritableTexture2D` members (stage 3 may add a float4 image — a span of
`width × height × 4` floats — so `CoverageKernel` and `PaintPlasma` run whole;
until then a plan that meets one is invalid). Render graphs (`Input`,
`Varying`, position/fragment) and `dfdx`/`dfdy`/`fwidth`. Packed half/bf16
`SimdMatrixLoad` until stage 3 brings the helpers that widen them; float
fragments first. *As built (stage 3):* the packed loads are in; the float4
image is not, so textures stay out.

## 3. Files

| File | Change | Lines |
| --- | --- | --- |
| `Lib/eacp/GPU/Codegen/ShaderMembers.h` | new: `Uniform<T>` and specialisations, `ShaderVisitor`, `ShaderBuildVisitor`, `ShaderUploadVisitor`, `CpuValueOf`, `ShaderValueOf`, `EACP_SHADER` macros — moved from `ShaderProgram.h`. *As built:* plus `ShaderValueIs`; the render-only visitors and vertex-format helpers stay behind | ~620 moved; *as built:* 718, ~695 moved |
| `Lib/eacp/GPU/Codegen/ShaderProgram.h` | includes `ShaderMembers.h`; loses the moved half | −620; *as built:* −695 |
| `Lib/eacp/GPU/Codegen/ComputeKernel.h` | new: the device-free base (D6) | ~680, mostly moved; *as built:* 591 |
| `Lib/eacp/GPU/Codegen/ComputeProgram.h` | `ComputeProgram : ComputeKernel`, GPU half only | −600; *as built:* 885 → 336 |
| `Lib/eacp/GPU/Codegen/ShaderGraph.h` | `isPure` public | ~5 |
| `Lib/eacp/GPU/Codegen/Codegen.h` | include `ComputeKernel.h` | ~1 |
| `Lib/eacp/GPU/Codegen/ShaderEmitter.cpp` | *as built (stage 2):* `floatLiteral` writes the shortest `%g` that reads back exactly | +18/−3 |
| `Lib/eacp/GPU/CMakeLists.txt` | `add_subdirectory(CpuCompute)` before the GPU return. *As built:* stage 0 needed no edit here — `add_ide_sources` globs headers | ~3 |
| `Lib/eacp/GPU/CpuCompute/CMakeLists.txt` | `eacp-cpu-compute`, force-optimised, `-fno-math-errno` | ~35; *as built:* 33; *stage 3:* +2 (`Helpers.cpp`, `SimdMatrix.cpp`) |
| `CpuCompute/CpuCompute.h` | umbrella | ~10; *as built:* 11; *stage 3:* 12 (`Helpers.h`) |
| `CpuCompute/Executor.{h,cpp}` | the public object: plan + workspace, dispatch forms, group iteration, uniform read-in | ~400; *as built:* 61 + 263; *stage 2:* 77 + 348; *stage 3:* `Executor.cpp` +6/−2 (`executorClearGroupMemory` zeroes the fragments too) |
| `CpuCompute/Bindings.h` | the slot table and typed setters | ~120; *as built:* 118 |
| `CpuCompute/CpuUniformVisitor.h` | the fourth visitor | ~70; *as built:* 48 |
| `CpuCompute/Plan.{h,cpp}` | graph → decoded ops, scratch layout, nesting depth, validation. *As built:* plus the per-statement schedules (D4) | ~650; *as built:* 299 + 1488; *stage 2:* 324 + 1829; *stage 3:* 373 + 2162 (+50/−1, +338/−5) — `Op::Helper`, `HelperFunction`, the fragment `Step` fields and layout, the statement checks |
| `CpuCompute/Workspace.{h,cpp}` | scratch, variables, shared, arrays, fragments, masks, epochs. *As built:* no epochs | ~200; *as built:* 34 + 60; *stage 2:* 35 + 73 |
| `CpuCompute/Interpreter.h` | *as built:* new, internal — `Context`, `SlotView`, mask frames | 67; *stage 2:* 102; *stage 3:* +15 |
| `CpuCompute/Evaluate.cpp` | the expression kinds over lane arrays, broadcast rules | ~900; *as built:* 537; *stage 2:* 606 |
| `CpuCompute/Statements.cpp` | block walk, masks, stores, records, atomics, reductions | ~550; *as built:* 198; *stage 2:* 396; *stage 3:* +16 — the four fragment statements |
| `CpuCompute/Builtins.cpp` | the `Call` set, conversions, bitcasts | ~550; *as built:* 667; *stage 3:* +127 — `builtinHelper` |
| `CpuCompute/Lanes.h` | lane-array primitives (D5) | ~300; *as built:* 172 |
| `CpuCompute/Helpers.{h,cpp}` | C++ twins of the 17 `eacp*` helpers (stage 3) | ~350; *as built:* 64 + 219 |
| `CpuCompute/SimdMatrix.cpp` | fragments per SIMD group (stage 3). *As built:* plus the packed loads' widening | ~200; *as built:* 249 |
| `Tests/CMakeLists.txt` | `add_subdirectory(CpuCompute)` | ~3 |
| `Tests/CpuCompute/CMakeLists.txt` | `CpuComputeTests`, `CpuComputeBench` | ~30; *as built (tests only):* 17; *stage 2:* +1/−1; *stage 3:* +2/−1 |
| `Tests/CpuCompute/ExecutorTests.cpp` | semantics: masks, loops, records, D7 rows, no-allocation | ~600; *as built:* ~2960 — every case carries an explicit C++ twin; *stage 2:* 3174; *stage 3:* 3258 |
| `Tests/CpuCompute/KernelTests.cpp` | `Apps/GPU` kernels as `ComputeKernel`s vs references | ~350; *as built:* 415 |
| `Tests/CpuCompute/GroupTests.cpp` | stage 2: shared, reductions, atomics, indirect | ~450; *as built:* 1517; *stage 3:* 1545 |
| `Tests/CpuCompute/HelperTests.cpp` | stage 3: helpers, fragments. *As built:* helpers only | ~350; *as built:* 1004 |
| `Tests/CpuCompute/SimdMatrixTests.cpp` | *as built (stage 3):* new — fragments, packed loads, the multiple-of-32 refusal | 820 |
| `Tests/CpuCompute/CpuComputeBench.cpp` | stage 4 | ~300 |
| `Tests/GPU/CMakeLists.txt` | `eacp-cpu-compute` on `GPUCodegenTests` and `GPUTests` | ~4; *as built (stage 1, `GPUTests` only):* +5/−2; *stage 2 (`GPUCodegenTests`):* +1/−1 |
| `Tests/GPU/CpuCrossCheck.h` | run-both-and-compare helper | ~180; *as built:* 348; *stage 2:* 488 — atomic outputs, `agreeing`, `runIndirect`, `expectAgreement`, `dispatchIndirectOnCpu` |
| `Tests/GPU/*Tests.cpp` (the suites in stages 1–3, ~22 files) | a CPU half per kernel | ~+40 each; *as built (stage 1, 13 files):* +859/−1181; *stage 2 (10 files):* +700/−993; *stage 3 (6 files):* +833/−1266 |
| `Tests/GPU/*CodegenTests.cpp`, `CodegenCommon.h` | numeric checks on the recorded graphs | ~+200; *as built (stage 2, 4 files):* +524/−107; *stage 3 (`SimdMatrixCodegenTests.cpp`):* +300/−56 |
| `Tests/GPUWidgets/…` | path kernels cross-checked (stage 2) | ~150; *as built:* `PathKernelCpuTests.cpp` 678, `CpuPathKernels.h` 190, `PathShapes.h` 50 (moved out of `CoverageBatchTests.cpp`), `PrefixSumTests.cpp` +203 |
| `Lib/eacp/GPUWidgets/Path/PathRasterizer.h` | *as built (stage 2):* `getSegments()`, `getTileCount()` public, so a test can gather a batch's inputs | +8/−4 |
| `Apps/GPU/CpuCompute/` | stage 5 example | ~200 |
| `Lib/eacp/GPU/README.md`, `CLAUDE.md`, `README.md` table | stage 5 | ~150 |

## 4. Stages

Each stage is one merge, green on macOS, Windows and all three Linux lanes.

**Stage 0 — the device-free seam.** D6 and nothing else: `ShaderMembers.h`
out of `ShaderProgram.h`, `ComputeKernel.h` out of `ComputeProgram.h`,
`isPure` public. No behaviour change and no kernel struct edited. Verified by
every existing suite matching its baseline on macOS and on the Linux lanes
(`GPUCodegenTests`, `GPUTests`, `GPUWidgetsTests`, `UITests` — anything that
records a kernel), and by one new `GPUCodegenTests` case that derives a
struct from `ComputeKernel`, constructs it, and checks its `emitMetal(graph())`
against the same body written on a bare `ShaderBuilder` — which only links if
the seam holds, since `GPUCodegenTests` has no `eacp-gpu`. Done when that
case is green on the Linux GCC lane. ~40 lines new, ~1,300 moved. *As
built:* the case is `ComputeKernel/recordsTheBodyABareBuilderDoes` in
`Tests/GPU/ComputeKernelCodegenTests.cpp` (58 lines), checking `emitMetal`,
`emitHlsl`, `emitGlsl`, `source().source` and `dispatchRank()` against the bare
builder, plus `expectGlslCompiles` where `eacp-spirv` is built; 5 tracked files
changed (+23/−1263) beside the three new ones.

**Stage 1 — tier one.** D1–D5, D7 (minus helpers), D9, D10 for the direct
forms: `eacp-cpu-compute` with `Constant`, `Uniform`, `Construct`, `Swizzle`,
`Call` (the full math set, conversions and bitcasts), `Unary`, `Binary`,
`Compare`, `Select`, `VarRead`, `Mul`, `ThreadId`, `BufferRead`,
`BufferVectorRead`, `ArrayRead`, `GridExtent`; statements `Declare`, `Assign`,
`If`, `Loop`, `Break`, `Continue`, `Store` (with record stores),
`VectorStore`; the bounds guard; 1D, 2D and 3D. A plan meeting anything else
is invalid with a reason. Verified by:

- `CpuComputeTests`: the executor's semantics case by case — divergent `If`,
  loops with per-lane trip counts, `Break`/`Continue` in nested loops, a
  record store that reads its own destination, every D7 row, a UInt constant
  above `INT_MAX`, a partial last group in each rank, a case per
  sign-sensitive operation of D2 with negative operands so `Int` is never
  silently treated as `UInt` — and the three kernels
  of `Apps/GPU/Compute` (`ToneKernel`, `CrossfadeKernel`, `SmoothKernel`),
  `AsyncCompute`'s `MixKernel` and `ComputeParticles`' `IntegrateParticles`,
  re-declared as `ComputeKernel`s with identical bodies, against inline
  references. One case replaces global `operator new`/`delete` in the binary
  with counting ones and asserts that a dispatch after the first performs
  zero allocations.
- `GPUTests`: a CPU half for the kernels of `GPUSmokeTests`, `IntrinsicTests`,
  `SelectTests`, `UIntBufferTests`, `UIntBufferVectorTests`,
  `UIntScalarTests`, `UIntVectorTests`, `Dispatch3DTests`,
  `ThreadIndexVectorTests`, `HoistingTests`, `InPlaceRecordTests`,
  `StorePlacementTests`, `InPlaceComputeTests` and `ComputeBufferRangeTests`
  through `CpuCrossCheck.h`, against the same references and tolerances as the
  GPU half. The CPU half runs on every lane; the GPU half where a device is.

Done when all of those are green everywhere, including the two Linux lanes
with no device, which is the first time they check a kernel's numbers.
~3,800 lines, ~1,500 of them tests. *As built:* `InPlaceComputeTests` is
entirely stage 3 — its only kernel calls `erf` — and so are `IntrinsicTests`'
`SaturatingTanh`, `ErrorFunction` and `VectorIntrinsic`; the five kernels
that needed the threadgroup tier (`UIntBufferTests::BinKernel`,
`ComputeBufferRangeTests::BumpKernel`, `UIntVectorTests::SharedPairKernel`,
`Dispatch3DTests::GroupIdVolumeKernel`,
`ThreadIndexVectorTests::RebuiltPairKernel`) got their CPU halves in stage 2,
and the helper deferrals — `InPlaceComputeTests` and `IntrinsicTests`'
three — in stage 3. The
five example kernels match their references *exactly*: the same `std::`
transcendentals, the same order of operations, no contraction. The
zero-allocation case replaces all 20 global `operator new`/`delete` forms in
its translation unit, so no other `CpuComputeTests` source may. `CpuCrossCheck`
is namespace `eacp::GPU::CrossChecks`: `CrossCheck {kernel}` with chainable
`.input(member, values, first = 0, count = -1)` and `.output(member, initial)`
or `.output(member, elements, fill)`, then `.run(count | w, h | w, h, d,
verify)` taking a trailing `std::source_location`; `Readback::name()` is
`"cpu"` or `"gpu"`; a free `dispatchOnCpu(kernel, bindings, extents...)` serves
the hand-bound cases. The planned `inOut` became `output(member, initial)`.
A post-implementation review added: caller buffers moved through
`std::memcpy` on a `std::byte*` view rather than aliased as `uint32_t`;
plan-time refusals for a store into a read-only slot or of the wrong family, a
`Declare`/`Assign` of the wrong type, an out-of-range or twice-reached block,
an out-of-range `ThreadId` axis, and a group above 2^20 lanes or a scratch
above `INT32_MAX` words (`ShaderGraph` gained `statementCount()`/`blockCount()`
for it); operand and output pointers hoisted out of the matrix and geometric
lane loops.

**Stage 2 — tier two: the threadgroup.** `LocalId`, `GroupId`, `SharedRead`,
`SharedStore`, `Barrier`, `GroupReduce` (both scopes, the fallback's fold
order), `AtomicAdd`, `AtomicLoad`, `SimdGroupIndex`; custom group shapes of
any size; `dispatchIndirect`. Verified by `CpuComputeTests`' `GroupTests.cpp`
(a barrier inside a loop, a reduction under divergence per D7, atomics
returning distinct slots, an indirect dispatch whose arguments another CPU
dispatch wrote); by CPU halves in `SharedMemoryTests`,
`ThreadGroupSizeTests`, `GroupReductionTests`, `AtomicTests` and
`IndirectDispatchTests`; by `GPUCodegenTests` running the graphs of
`AtomicCodegenTests` and `GroupReductionCodegenTests`; and by the
`GPUWidgets/Path` kernels — `ScanBlockKernel`, `ScanAddKernel`, `ClearKernel`,
`BinKernel`, `BackdropScanKernel` — run on the CPU over the inputs
`GPUWidgetsTests` already builds and compared against the GPU's buffers on a
device (`CoverageKernel` minus its texture write waits for stage 3). Done when
the path pipeline's bins and backdrops agree to the element with the GPU on
macOS and on lavapipe. ~1,500 lines, ~800 of them tests.

*As built:* everything listed plans and runs; nothing in the stage-2 suites,
the codegen graphs or the five path kernels was refused. The library grew
+669/−32 across 9 files; the tests +4,138/−1,221, the deletions being
duplication the shared cross-check helpers folded away. `Executor/barrierMakesThePlanInvalid`
became `Executor/aBarrierDropsTheGuardAndStoresStayInBounds`, and the stage-2
rows of `everyOutOfTierConstructIsRefusedByName` moved to
`Group/malformedGroupStatementsAreRefusedByName`.
`Executor/aGroupDispatchAfterTheFirstAllocatesNothing` extends D3's
zero-allocation check to shared memory, a barrier, `groupSum`, `simdMax`,
atomics and the indirect form. `GPUTests` stays at 478: the CPU halves went
into existing cases — `SharedMemory` `everyThreadReadsAnotherLane`, five of
`ThreadGroupSize`, all nine of `GroupReduction`, `Atomic`'s ticket and bucket
cases, all four of `IndirectDispatch` (three run Count → Prepare → indirect
Consume on the CPU; the offset case at 4, −4, 8, 16 and 64 bytes) — plus the
five stage-1 deferrals. `SharedMemoryTests`' `BudgetedKernel`,
`WideElementKernel` and `OverBudgetKernel` are never dispatched and have no CPU
half. `GPUCodegenTests` gained ten `…/runs` cases over `AtomicCodegenTests`'
and `GroupReductionCodegenTests`' graphs. `GPUWidgetsTests` gained
`PathKernelCpuTests.cpp` (5 cases over the mixed, very-different-backdrops and
24-star batches) and three `PrefixSumTests` cases; the chain runs clear →
count → backdrop → sum → fill, each stage its own command buffer, every buffer
compared after each with 5 sentinels past every array. Integer buffers match
exactly; fill-mode entries are compared per tile as sorted sets, since slot
order inside a tile follows the GPU's atomic cursor. The backdrop scan is also
run over the GPU's own crossings, and the CPU's crossings and backdrops are
held to a plain C++ reference on every lane.

That cross-check caught the first real bug of the plan, and it was in the
emitter, not the executor: `floatLiteral` printed `%g`, six significant
digits, so `backdropFixedScale` = 2^20 reached every dialect as `1.04858e+06`
= 1048580 and the GPU's crossings drifted 1 to 4 units (1737 of 14882 cells in
the mixed scene) — a C++ single-precision copy matched the executor at 2^20
and the GPU at 1048580. It now writes the shortest `%g` (6 to 9 digits) that
`strtof` reads back exactly; a value exact at six digits keeps its spelling,
so no golden string changed, and 2^20 is `1048576.0`.
`GPU/codegenFloatLiteralRoundTrips` guards it (the 115th codegen case), and
`PathKernelCpuTests`' crossing and whole-chain backdrop comparisons, gated
while the bug stood, now run unconditionally and match Metal to the element.
Not yet done:
the lavapipe half of "done when", which waits for CI.

**Stage 3 — tier three: packed data and the SIMD-group matrix.**
`CpuCompute/Helpers.{h,cpp}`: the 17 `eacp*` helpers in C++ (half with
round-to-nearest-even and the denormal and infinity cases, bf16, int8/uint8,
int4/uint4 unpack, the int8/uint8 packs, `eacpErf`/`eacpErfc` as the same
approximation the shader source spells, `eacpSaturatingTanh`), used by the
executor and by the tests. `SimdMatrixFill`, `SimdMatrixLoad`,
`SimdMatrixStore`, `SimdMatrixMultiplyAdd` in float, then the packed half and
bf16 loads through the helpers. Optionally a float4 image binding for
`WritableTexture2D` so `CoverageKernel` and `ComputeImage`'s `PaintPlasma` run
whole. Verified by CPU halves in `PackedHalfTests`, `PackedBFloat16Tests`,
`PackedQuantizedTests` and `SimdMatrixTests`, by `GPUCodegenTests` running
`SimdMatrixCodegenTests`' graphs, and by `HelperTests.cpp` checking each
helper exhaustively where the domain allows it (all 65,536 halves and bf16s,
all 256 bytes) against a double-precision reference. Done when those suites'
CPU halves pass on every lane and their GPU halves still pass. ~1,200 lines.

*As built:* everything listed but the optional image binding. The library
grew +557/−9 across 9 tracked files plus `Helpers.{h,cpp}` (64 + 219) and
`SimdMatrix.cpp` (249); the tests +1,273/−1,349 across 10 tracked files plus
`HelperTests.cpp` (1004) and `SimdMatrixTests.cpp` (820), the deletions being
the per-test helper copies. The helpers (D3, D7) and the fragments (D2) were
built in parallel and merged; the fragments' tests went to
`Tests/CpuCompute/SimdMatrixTests.cpp` rather than `HelperTests.cpp`, and the
zero-allocation fragment case, `Executor/aFragmentDispatchAfterTheFirstAllocatesNothing`,
to `ExecutorTests.cpp`, because that file owns global `operator new`.
`HelperTests.cpp` checks every half and bf16 pattern and every byte and
nibble, and runs each helper through the executor on scalar and vector lanes.
`Executor/everyOutOfTierConstructIsRefusedByName` lost its `eacpErf` row to
`Executor/aMalformedHelperCallIsRefusedByName`, and
`Group/malformedGroupStatementsAreRefusedByName`'s `SimdMatrixFill` row
became three raw-graph refusals. `SimdMatrix/aGroupOfPartialSimdGroupsIsRefused`
pins D2's multiple-of-32 refusal at 48 and 16 threads.

`GPUTests` stays at 478: the CPU halves went into existing cases —
`PackedHalfTests`, `PackedBFloat16Tests`, `PackedQuantizedTests`, the three
helper cases of `IntrinsicTests`, `InPlaceComputeTests`, and every
`SimdMatrixTests` case that dispatches a kernel: the four float ones through
`CrossCheck`, the two packed products and
`aMixedProductDoesNotNarrowTheFloatOperand` bound by hand through
`dispatchOnCpu`, since their GPU halves still skip unless the device holds the
packed type natively and the CPU half must not. The packed operand is bound as
the float array holding its words. `aPackedLoadIsRefusedWhereTheDeviceSaysNo`
and `aRefusedKernelDispatchesNothing` have no CPU half: they check the
device's answer and `ComputePass` dropping a dispatch, and the executor asks
the device nothing — the packed products' CPU halves already run where it says
no. `IntrinsicTests`, `PackedHalfTests` and `PackedBFloat16Tests` replaced
their helper copies with `Helpers.h`; `PackedQuantizedTests` keeps its own
layout references, independent of the twins. `TiledProduct::dispatch` became
`setShape`/`gridWidth`/`gridHeight` so one kernel serves both backends, and
the encoder-shape case, ~900M multiply-adds, runs on the CPU in under a
second. `GPUCodegenTests` gained four `…/runs` cases over
`SimdMatrixCodegenTests`' graphs: `eachBackendSpellsItsOwnWay/runs`,
`aFragmentTakesTheBoundsGuardAway/runs`, and `bfloat16LoadsWithoutStaging/runs`
and `halfLoadsWithoutStaging/runs` against a reference through the helpers
(`packedKernel` became a `PackedKernel` struct for its handles). Two of those
graphs read tile memory nothing wrote — undefined on a GPU — and the cases say
they rely on the CPU's zero fill.

Verified by breaking the executor on purpose: `Op::Helper` computing nothing
failed exactly 39 `GPUTests` cases, all on the CPU half; the product's right
operand transposed failed all 9 product cases across the three binaries; the
packed parity swapped failed exactly the 5 packed-load cases (1
`CpuComputeTests`, 2 `GPUCodegenTests`, 2 `GPUTests`). Not done: the float4
image binding for `WritableTexture2D`, so `CoverageKernel` and `PaintPlasma`
still wait; the emitter's multiple-of-32 rule (D2) is still a Debug-only
assertion.

**Stage 4 — performance.** `CpuComputeBench` beside the tests, force-optimised
outside Debug as `SimdBench` is, timing the interpreter against hand-written
C++ loops compiled with the same flags: the three `Compute` kernels and
`MixKernel` over 1M elements, `BinKernel` over the `PathBench` scene. Then, in
order of what the benchmark says: scratch-slot reuse by liveness;
lane-invariant nodes evaluated once per group or dispatch; cross-statement
hoisting of pure nodes; wide batches (several groups per batch for a kernel
with no group-scope feature, so a 64-lane group stops bounding the loop
length); intrinsics for the gather, the masked blend and the transcendentals
(D5); `dispatchGroups` for caller-supplied threads (D8). The target is within
3x of the hand-written loop on the stream kernels and within 10x on
`BinKernel`; both are targets, not promises, and the stage records what it
reached. Verified by the benchmark's numbers in the Progress table and by
every earlier suite still green (the optimisations must not change a result
bit: same order of operations, no contraction).

**Stage 5 — integration.** The cross-check on in CI: nothing to switch, since
the CPU halves always run and the GPU halves run wherever a device is (the
lavapipe lane, which is the only one `build.yml` sets `EACP_REQUIRE_GPU` on,
and macOS and Windows wherever the runner has one, as `GPUTests` does today);
an `EACP_CPU_CROSSCHECK=0` escape only if a lane needs it. `Lib/eacp/GPU/README.md` gets a "Running a kernel on the CPU"
section after the compute sections (the `ComputeKernel`/`ComputeProgram`
split, `Bindings`, the dispatch forms, the D7 table, the realtime contract and
what breaks it); `CLAUDE.md` a paragraph beside `eacp-gpu-codegen`'s; the
platform table in `README.md` a row. `Apps/GPU/CpuCompute`: one kernel
dispatched on both, the results compared and timed, and the CPU path alone
where no device came up. Done when the example runs on macOS, Windows and a
driverless Linux box.

## 5. Risks and open questions

- **Interpreter overhead and masked-lane waste.** A 64-lane batch amortises
  one dispatch per node over 64 lanes, which is fine for streams and poor for
  `BinKernel` and `CoverageKernel`, whose inner loops run per-lane trip counts:
  the batch runs as long as its longest lane with the rest masked. If stage 4
  shows it, compacting live lanes between iterations is the next step, and is
  its own design.
- **Barriers in nested, divergent control flow.** Lockstep is exact while
  every lane of the group reaches each barrier together, which is the only
  case the GPU defines. Under divergence the CPU treats the barrier as a no-op
  (D7) and the README says so; a kernel that relies on anything else is
  already broken on the GPU.
- **Broadcast in `Binary` and `Call`.** The emitter leaves scalar/vector
  mixing to the target language; the plan must apply the language's rule
  (a one-component operand broadcasts) per operand of every `Binary`,
  `Compare`, `Select` and multi-argument `Call`. A missed case is a silent
  wrong answer, which is what the per-kind tests in stage 1 are for.
- **NaN, infinity and denormals.** Metal's fast math may assume no NaN and
  flushes denormals; the CPU is IEEE. Tests avoid inputs where that matters.
  Denormals are also a speed hazard on x86 in an audio thread; whether the
  executor sets FTZ/DAZ for the length of a dispatch (restoring them after,
  since an audio host has its own opinion) is open — it matches the GPU
  better and costs two register writes.
- **Precision of vectorised transcendentals** (stage 4). A vector `sin`/`exp`
  is only acceptable within the tolerances the GPU tests already grant the
  GPU's own; anything looser stays `std::`.
- **Scratch footprint.** One slot per node is 512 KB at 500 nodes × 64 lanes
  × 4 components; a 256-thread group with `Float4x4` nodes is sixteen times
  that. Fine for tests and most kernels, heavy for an audio plugin with many
  kernels; liveness reuse in stage 4 is the answer, and the plan reports its
  footprint so a caller can see it.
- **The stage-0 refactor.** 54 files name `ComputeProgram` and 158 structs
  derive from it; D6 is designed so none of them changes, and the moved code
  is header-only. What could still bite is a translation unit that relied on
  `ShaderProgram.h` pulling in `Device.h` transitively for something
  unrelated — the compiler will say so, and the fix is an include.
- **`maxBufferSlots = 8`.** `Bindings` is a fixed table so binding never
  allocates. The CPU has no reason for the limit, but a kernel that exceeds it
  cannot run on the GPU, and matching `ComputePass` keeps a kernel honest.
  Keep it unless a CPU-only user asks.
- **Binding object versus a span in `Uniform<InputBuffer>`.** D6 picks the
  separate `Bindings`. If kernels end up always bound both ways at once,
  carrying both in the member is the convenience to reconsider; it is
  additive and does not change the executor.
- **Races made deterministic.** Lockstep with commit-after-evaluate turns an
  intra-group race into a defined result, and serial groups make cross-group
  ordering look sequential. A kernel can therefore pass on the CPU and fail on
  the GPU; the cross-check exists to catch that, and the README says the CPU
  is not a race detector.
- **Aliased bindings.** The same memory bound to an input slot and an output
  slot diverges from the GPU: the GPU keeps a read hoisted across a store
  (`InPlace/aReadIsNotRefreshedByAnotherSlotsStore`), where the CPU re-reads
  per statement. The README (stage 5) documents such a binding as unsupported
  on both.
- **`GPUCodegenTests` growing a numeric side.** Linking `eacp-cpu-compute`
  there (D9) keeps it device-free, and turns its recorded graphs into value
  checks on every lane; the cost is that a codegen test can now fail for an
  interpreter bug. Its numeric cases are named `…/runs` so which half failed
  is visible in the name.
- **Uniform read-in on every dispatch.** Walking `reflectMembers` per dispatch
  is a handful of virtual calls — negligible — but it reads `Uniform::value`
  from whatever thread dispatches, so a caller setting uniforms on one thread
  and dispatching on another needs its own synchronisation, exactly as with
  any plain member.
