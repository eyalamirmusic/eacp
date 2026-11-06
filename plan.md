# Shader EDSL: gaps against WGSL

The C++ shader EDSL (`Lib/eacp/GPU/Codegen`) records a `ShaderGraph` and
emits MSL, HLSL and GLSL. Measured against WGSL, the WebGPU shading
language, it is missing the items below. Every one of them is reachable
today from a hand-written `ShaderSource` (`msl`/`hlsl`/`glsl`), so these
are gaps in what the EDSL can express, not in what the backends can run.

Status after each item: **done**, **partial** (a narrower form exists,
described) or **absent** (nothing). Paths are under `Lib/eacp/GPU/Codegen/`
unless given.

Where it stands: stages 1–8 of "Suggested order" are done, and part A of
stage 9 (non-square matrices, private arrays, i32 buffers). Open: part B of
stage 9 (f16 values and matrices, emitted structs and struct buffers) and
stages 10–13. What each done stage left behind is listed under it and in its
section; "Verification" at the end says what has and has not been run.

## 1. Stage built-ins

`ShaderBuiltIn` (`ShaderGraph.h`) was `FragmentCoordinate`, `VertexId` and
`InstanceId`; `stageOf`/`builtInType` now say whose each is, and the
emitters assert a stage reads only its own.

- `front_facing` — **done**: `frontFacing()` (`Bool`), a parameter on Metal
  and HLSL.
- `frag_depth` output — **done**: `setFragmentDepth(Float)` /
  `ShaderBuilder::fragmentDepth`. With it (or a mask) the fragment returns a
  `FragmentOut` struct on Metal and HLSL; GLSL assigns `gl_FragDepth`. No
  pipeline flag: depth only means something with a depth attachment.
- `sample_index` — **done**: `sampleIndex()`. Vulkan now enables
  `sampleRateShading` where the device has it; there is no device query for
  it yet.
- `sample_mask` in and out — **done**: `sampleMask()` and
  `setSampleMask(UInt)` / `fragmentSampleMask`, both in one entry point.
- `local_invocation_index` — **done**: `localIndex()`.
- `num_workgroups` — **done**: `groupCount()` (`UInt3`). Native on Metal
  (at gid's width, so a 1D/2D kernel reports 1 on the axes it lacks) and
  Vulkan; HLSL derives it from the extent uniforms and the group shape,
  exact for direct dispatch, the guard count's groups for
  `dispatchIndirect(program, ...)`. Exact D3D indirect needs a command
  signature feeding the arguments in as root constants too — deferred.
- `subgroup_size`, `subgroup_invocation_id` — **done** as
  `simdGroupSize()`, `simdLaneIndex()`: native on Metal, `lane % 32` and
  `32` on HLSL (FXC SM5 has no `WaveGetLaneIndex`) and GLSL, matching the
  emulated SIMD group the `simd*` folds use. Width is still the fixed
  `simdGroupWidth = 32`.
- Subgroup shuffle, ballot and broadcast — absent (`simdSum`/`Max`/`Min`
  are the only subgroup ops, emulated with barriers off Metal). Deferred
  from stage 2: off Metal they need either SM6/`GL_KHR_shader_subgroup_*`
  or a threadgroup-memory emulation like the folds.

## 2. Types

`ValueType` (`ShaderTypes.h`) has bool, i32, u32, f32 and their 2–4
vectors with every swizzle. Missing:

- `f16` as a value type — absent (stage 9, part B). Only packed storage
  access exists: `packHalf2`/`unpackHalf2`, `readHalf*`, `writeHalf*`.
- Non-square matrices (`mat2x3` … `mat4x3`) — **done**: `Float2x3` …
  `Float4x3` (columns first; HLSL `floatRxC`), see step 9.
- `f16` matrices — absent (stage 9, part B).
- Mutable fixed-size private arrays — **done**: `privateArray<T, N>()` /
  `privateArray(values...)`, see step 9. `ConstantArray` unchanged.
- Runtime-sized arrays of i32 — **done**: `IntInputBuffer`/
  `IntOutputBuffer`. Of structs — absent (with emitted structs).
- Emitted structs — absent (stage 9, part B); C++ structs of handles exist
  at authoring time only, nothing is emitted and none can live in a buffer.
- Pointers and references — absent (no such `ExprKind`).
- `atomic<i32>` — **done**: `IntAtomicBuffer` (`atomic_int` /
  `RWStructuredBuffer<int>` / `int[]`), bound from `int32` (CPU
  `Bindings::set` takes `std::span<std::int32_t>`); `sharedAtomic<Int>`.

## 3. Address spaces and bindings

- Uniform buffers — partial: one packed `Uniform<T>` block per program;
  every matrix shape now crosses (step 9); `Bool` still refused by
  `static_assert` in `ShaderBuilder::uniform`.
- Read-write storage from vertex and fragment stages — partial:
  `OutputBuffer` is compute-only; `InputBuffer` is readable everywhere.
- Private variables — **done**: `var()` for scalars, vectors and matrices,
  `privateArray` for arrays of them.
- Push constants — partial: `setVertexBytes`/`setFragmentBytes`/`setBytes`
  on the API side; the EDSL sees them as the uniform block.
- Texture kinds — **partial**: `texture_3d`, `texture_cube_array`,
  `texture_depth_cube` and `texture_depth_cube_array` **done**
  (`Texture3D`, `TextureCubeArray`, `TextureDepthCube`,
  `TextureDepthCubeArray`, `Codegen/TextureFunctions.h`). D3D12's whole-plane
  depth SRV of a Cube/CubeArray target is now a cube view (was a 2D array).
  Absent, both API-side gaps: `texture_1d` (no `TextureType::OneD` on any
  backend), and `texture_multisampled_2d` / `texture_depth_multisampled_2d`
  (the MSAA companion of a target is render-target-only: no ShaderRead usage
  on Metal, no SRV/sampled view, no state transitions or bind on D3D12 and
  Vulkan).
- `Texture2DArray` / `TextureCube` — **done**: every function below.
- Storage textures — **done**: `StorageTexture2D/2DArray/3D<Format,
  StorageAccess>` (`Codegen/StorageTextures.h`), Write / Read / ReadWrite,
  every storable format, Float4/UInt4/Int4 texels. ReadWrite gated by
  `ComputeProgram::fitsStorageTextures` (`supportsComputeReadWrite`), Write by
  `supportsComputeWrite`; Read is the SRV/sampled path, no gate, fragment
  stage too. Left: Write/ReadWrite in the fragment stage (D3D12 render root
  signature has no UAVs, Vulkan needs `fragmentStoresAndAtomics`, no
  RenderPass bind), storage levels other than 0, `textureBarrier`.
- Samplers — **done**: `SamplerBinding` / `ComparisonSamplerBinding`
  (`Codegen/Samplers.h`), WGSL's `sampler` / `sampler_comparison`, taken by
  every sampling function after the texture; slots from the texture
  counter, bound with `RenderPass::setFragmentSampler` /
  `ComputePass::setSampler` (`makeSampler` samplers). The eight baked
  `TextureSampling` configurations stay, byte-identical.

## 4. Control flow

Statements (`var`, `ifThen`, `loop`, `switchOn`, `forLoop`, `returnEarly`,
`discard`) go to the fragment or kernel function, and to the vertex function
inside `vertexStage(body)`.

- Any statement in the vertex stage — **done**: `vertexStage(body)` records
  `var`/`ifThen`/`loop`/`break`/`continue` into a second root block
  (`ShaderGraph::vertexBlock()`); explicit-level texture reads and sampler
  bindings work there.
- `switch` — **done**: `switchOn(sel, caseOf(v, body), caseOf({v...},
  body), defaultCase(body))`, Int/UInt, no fallthrough; loop jumps in a
  clause carried out through a `jumpN` flag.
- `for` — **done**: `forLoop(cond, update, body)`, over the continuing loop.
- `break if` — **done**: `loop(cond, body, continuing)` + `breakLoopIf`.
- Early `return` — **done**: `returnEarly()` in all three stages.
- `discard` as a statement — **done**: `discard()`, fragment only;
  `discardBelow` unchanged. On Vulkan 1.3 GLSL's discard is
  `OpDemoteToHelperInvocation` (SPIR-V 1.6; the device now enables
  `shaderDemoteToHelperInvocation`), a helper as WGSL's is; `OpKill` on the
  Vulkan 1.1 floor (SPIR-V 1.3/1.4).
- Emitted user functions with parameters and return values — absent
  (stage 12); C++ functions are inlined into the graph.

## 5. Built-in functions

Present and complete: abs, clamp, mix, smoothstep, step, pow, exp, exp2,
log, log2, floor, ceil, round, trunc, fract, sign, min, max, sqrt, rsqrt,
length, distance, normalize, dot, cross, reflect, refract, faceforward,
determinant, transpose, select, all, any, sin, cos, tan, asin, acos, atan,
atan2, sinh, cosh, tanh, dfdx, dfdy, fwidth, sampleCompare, atomicAdd,
barrier. Missing:

- `asinh`, `acosh`, `atanh` — **done**: native on GLSL; HLSL helpers in
  WGSL's log forms; Metal's asinh a helper over `|x|` (fast math cancels).
- `fma` — **done**: `mad` on HLSL; scalars widened on MSL and GLSL.
- `degrees`, `radians` — **done**: a multiply by the constant, no call.
- `quantizeToF16` — **done**: `eacpQuantizeToF16` helper.
- `dpdxCoarse`/`dpdxFine`, `dpdyCoarse`/`dpdyFine` — absent.
- `pack4x8unorm`, `pack4x8snorm`, `pack2x16unorm`, `pack2x16snorm` and
  their unpacks — **done** as `packUnorm8x4`, `packSnorm8x4`,
  `packUnorm16x2`, `packSnorm16x2`, `unpack…`: helpers with WGSL's
  `floor(0.5 + x)`, not the dialects' own packs.
- `countOneBits`, `reverseBits`, `firstLeadingBit`, `firstTrailingBit`,
  `countLeadingZeros`, `countTrailingZeros`, `extractBits`, `insertBits` —
  **done**: `eacp*` helpers on Int/UInt and their vectors, WGSL's clamping
  and -1/0xFFFFFFFF answers; offset/count are `UInt` or literals.
- `arrayLength` — absent (`gridCount` is the stand-in for the dispatch, not
  for a bound buffer).
- `textureSampleBias`, `textureSampleGrad`, `textureSampleLevel` on every
  kind, `textureSampleCompareLevel`, `textureGather` (with component),
  `textureGatherCompare`, `textureLoad` at a level on 2D, arrays, 3D and
  depth, `textureDimensions` (with level), `textureNumLevels`,
  `textureNumLayers` — **done**: `ExprKind::TextureFunction` /
  `TextureQuery`; HLSL queries through one `eacpTextureInfo<Kind>` helper
  per kind (GetDimensions has out-params); comparisons level-zero
  everywhere. Implicit-derivative forms assert in a kernel. Left:
  `textureNumSamples` and MS loads (no MS kinds), offset variants, the CPU
  executor (no texture reads at all), and the old `sample(Texture2D)` in a
  kernel still emits HLSL `Sample` (invalid in cs_5_0).
- `textureStore` on anything but a 2D float4 target — **done**: `write` on
  storage textures, with `load`, `dimensions`, `layerCount`.
- Atomics: `atomicSub`, `atomicMax`, `atomicMin`, `atomicAnd`, `atomicOr`,
  `atomicXor`, `atomicExchange`, `atomicCompareExchangeWeak`; atomics on
  workgroup memory — **done**: one `Atomic` statement with an
  `AtomicOperation`, on u32/i32 buffers and `sharedAtomic<T>`; Sub is an add
  of the negation on HLSL/GLSL; compare-exchange returns
  `CompareExchangeResult<T>{oldValue, exchanged}`, weak on Metal, strong
  elsewhere. Shared atomics start uninitialised like `shared<T>` (no WGSL
  zeroing). Unverified: the HLSL, by documentation only.
- `storageBarrier`, `textureBarrier` — absent (`barrier()` is
  `workgroupBarrier` only).

## 6. Pipeline-overridable constants

`override` and specialization at pipeline creation — absent (stage 11). No
`MTLFunctionConstantValues`, no `VkSpecializationInfo`, no DXC `-D`
equivalent. The current form is C++ constructor arguments that rebuild the
graph, with `visit<Shader>(args)` variants for precompilation.

## 7. Interpolation qualifiers

All open (stage 10).

- Integer varyings are flat automatically (`flat` / `nointerpolation` /
  `[[flat]]`).
- User-chosen `flat` on float varyings — absent.
- `linear` (no perspective) — absent.
- `centroid`, `sample` — absent.
- Bool varyings — refused.

## 8. Compilation and reflection

All open (stage 13).

- Structured compile errors — partial: `ShaderLibrary::isValid()` plus a
  logged compiler message (`Shader/ShaderLibrary-Windows.cpp`,
  `-Linux.cpp`, Metal `loadFailureOf`); `Dxc::status().error`; no
  line/column/message object.
- Reflection — partial: the `ShaderGraph` itself and
  `GeneratedShader::vertex/fragmentReadsUniforms`. `ShaderSource::bindings`
  and `ResourceBinding` exist but nothing fills them and no backend reads
  them (dead plumbing).

## Housekeeping found during the audit

- `ShaderValue.h` has a comment saying `GPU::Texture` has no mips yet;
  mipmaps exist. **Done.**
- `CommandBuffer/CopyPlan.cpp` refuses copies with depth and says eacp
  lacks 3D textures; `TextureType::ThreeD` exists. **Done** (wording only;
  a buffer-texture copy with depth is still refused).
- `ShaderSource::bindings` / `withBinding` / `ResourceBinding` are unused.
  Open (stage 13).

## Suggested order

1. **Cheap scalar built-ins** — asinh/acosh/atanh, fma, degrees/radians,
   quantizeToF16, the bit functions, the unorm/snorm packs. **Done**: call
   nodes and `eacp*` helpers (no new `ExprKind`), CPU twins in
   `CpuCompute/Helpers.h`; `WgslBuiltins/`, `WgslHelpers/`,
   `WgslIntrinsics/` suites.
2. **Stage built-ins** — front_facing, frag_depth, sample_index,
   sample_mask, local_invocation_index, num_workgroups. Same shape: an
   enum value and three spellings. **Done**, with subgroup size and lane;
   CPU executor runs the kernel four. `StageBuiltInCodegen/`,
   `KernelBuiltIns/`, `KernelBuiltInDevice/`, `FragmentBuiltIn/` suites.
   Left: subgroup shuffle/ballot/broadcast, exact D3D indirect
   `groupCount`.
3. **Atomics** — the remaining ops on u32, then i32 and workgroup memory.
   The CPU interpreter needs each op too. **Done**: `StatementKind::Atomic`
   replaces `AtomicAdd`; templated ops over `AtomicStorage` targets with
   handle-or-literal `AtomicOperand`s; CPU buffer atomics are
   `std::atomic_ref` (stores into atomic slots too), shared ones plain.
   Also fixed: an `INT_MIN` literal prints `(-2147483647 - 1)`.
   `AtomicOpsCodegen/`, `AtomicOps/`, `AtomicOpsDevice/` suites. Left:
   `atomicLoad`/`atomicStore` are plain subscripts on HLSL/GLSL (as before);
   no zeroing of workgroup memory on any backend.
4. **Texture functions** — dimensions and counts, sample-at-level
   everywhere, bias, grad, gather, load at a level. **Done** with the 3D,
   cube-array and depth-cube kinds; `TextureFunctionCodegen/`,
   `TextureFunctionDevice/` (Metal, and lavapipe since, the Vulkan
   cube-array layer count included). Deferred: 1D and multisampled kinds
   (API side, see §3), offsets, CPU texture reads; stores are stage 5.
   Unverified: HLSL by documentation only, the D3D12 cube depth SRV.
5. **Storage textures** — read and read-write `WritableTexture2D`, arrays,
   3D, formats beyond float4. **Done**: format and access in the type, slot
   format on the graph (`ShaderGraph::textureFormat`), loads as `load`
   TextureFunction nodes, stores as `TextureStore` with a coordinate vector and
   `layer`. HLSL typed UAVs as wide as the format (`RWTexture2D<float>` for
   R32Float) and loads widened (0, 0, 1); Metal `fence()` after a ReadWrite
   store; GLSL qualifier on ReadWrite only (writes stay unformatted). Members
   bind only their exact format and shape, cubes as arrays. Vulkan now enables
   `shaderStorageImageExtendedFormats` and gates ReadWrite on it past SPIR-V's
   base formats. `WritableTexture2D` byte-identical. `StorageTextureCodegen/`,
   `StorageTextureDevice/` (Metal, and lavapipe since). Unverified: HLSL by
   documentation only (FXC cs_5_0 multi-component typed-UAV loads assumed to
   compile with the additional-formats flag, as `KernelImage/` already
   relies on). Deferred: fragment-stage writes, non-zero levels,
   `textureBarrier`, CPU storage textures.
6. **Samplers as bindings** — replace the eight baked `TextureSampling`
   configurations with a sampler binding so `makeSampler` samplers reach
   EDSL shaders. **Done**, additive: `TextureAccess::Sampler` slots from the
   texture counter, `Expr::sampler` on `TextureFunction` nodes (explicit
   forms are all TextureFunction nodes, `sample` included); handle type
   picks plain vs comparison at compile time. MSL `sampler [[sampler(n)]]`;
   HLSL `SamplerState`/`SamplerComparisonState` at `s<8 + n>`, the existing
   per-bind sampler tables (render and compute root signatures unchanged);
   GLSL `sampler`/`samplerShadow` at the slot's binding plus an aliased
   `texture<Kind> textureImage<m>` on the sampled texture's combined binding,
   `texture(sampler2D(textureImage<m>, sampler<n>), ...)`. Vulkan render keeps
   its fixed layout: a sampler-only slot writes the combined descriptor with
   the image half lent by a bound texture; compute reflects `OpTypeSampler`
   as `SAMPLER` (written from `setSampler`), lets `COMBINED` win over an
   aliasing `SAMPLED_IMAGE`, writes `SAMPLED_IMAGE` from sampled binds; pool
   gained both types. Members bind only a valid sampler of their own kind,
   else nothing (mismatch logged once). CPU refuses sampler slots by name.
   `SamplerBindingCodegen/` (full baked golden, checked identical against
   the pre-change library), `SamplerBindingDevice/` (Metal, and lavapipe
   since, the aliasing rule and lent image clean under validation layers),
   `Executor/aSamplerSlotIsRefusedByName`. Unverified: HLSL by
   documentation, D3D12 C++ uncompiled. Deferred: vertex-stage EDSL sampling
   (stage 7), CPU samplers, removing the baked configurations.
7. **Statements in the vertex stage** — the emitter change that unlocks
   vertex-stage texture sampling and control flow. **Done**: attributed by
   the stage open while recording (`vertexStage(body)` →
   `beginVertexStage`/`endVertexStage`, a lazily made vertex root block), not
   by reachability; each variable remembers its stage. Expressions stay
   stage-free and print in whichever function reaches them; a `VarRead`
   belongs to its variable's stage and may not cross (assert,
   `stagesReadOnlyTheirOwnVariables`; assignment outside the stage asserts).
   Only Declare/Assign/If/Loop/Break/Continue in the vertex stage (WGSL: no
   storage writes, atomics, barriers). Implicit-derivative reads - plain,
   layered and compare `Sample` nodes, `Sample`/`Bias`/`Compare`
   TextureFunctions, `dfdx`/`dfdy`/`fwidth` - assert in the vertex stage
   (`vertexUsesImplicitDerivatives`); level, grad, compare-level, gather,
   gather-compare, load and queries allowed. Metal vertex function declares
   the slots it reads; `GeneratedShader::vertexTextureSlots` (bitmask) drives
   `ShaderProgram::bindTextures` to also call `setVertexTexture`/
   `setVertexDepthTexture`/`setVertexSampler`. API side was already there
   (D3D12 SRV/sampler tables and static samplers `VISIBILITY_ALL`, Vulkan
   render layout VERTEX|FRAGMENT, Metal `setVertexTexture`/`SamplerState`);
   no backend change. `vertexReadsUniforms` and Metal vertex buffer params
   walk the vertex statements. Statement-free vertex output byte-identical
   (golden). `VertexStatementCodegen/`, `VertexStatementDevice/` (Metal, and
   lavapipe since, the Vulkan double bind included). Unverified: HLSL vertex
   `Gather`/`GatherCmp`/`SampleGrad`/`GetDimensions` under FXC vs_5_0 and the
   D3D12 double bind, by documentation only.
   Deferred: storage-texture writes and read-write buffers in the vertex
   stage, `switch`/early return there (stage 8); `doesReferenceUniform` still
   skips constant-array elements (pre-existing, both stages; fixed in stage 8,
   with the Metal buffer-parameter walk).
8. **Control flow** — switch, early return, general discard, break-if.
   **Done**: `StatementKind::Switch`/`Case` (labels in
   `ShaderGraph::caseLabels`, `BlockRole::Cases` holds only Cases),
   `BreakIf`, `Return`, `Discard`, `Statement::continuing` on a Loop;
   `BlockRole` on each block drives the WGSL asserts (break/continue/return
   not in a continuing, break-if only its last statement, discard not in
   the vertex stage, a kernel's at emission). Switch: braced clauses ended
   by `break;`; `breakLoop`/`continueLoop` in a clause mean the loop, so a
   switch with such a jump declares `int jumpN`, sets it and breaks, and
   re-takes the jump after the switch (through enclosing switches too) -
   HLSL refuses `continue` in a switch. Continuing loops print Naga's form:
   `bool firstPassN; while (true) { if (!firstPassN) { continuing } ...
   if (!cond) break; body }`, so `continue` runs the continuing; plain
   loops byte-identical. Render `returnEarly()` returns the stage's outputs
   evaluated where it stands (vars read are the values there; asserts they
   are in scope), braced `VertexOut`/`FragmentOut` on Metal/HLSL, alpha test
   repeated; kernel `return;` beside the bounds guard (return before a
   barrier/fold is UB, documented, unchecked). CPU: Switch steps over Case
   steps (labels in `Plan::caseLabel`), per-lane clause masks, default from
   the unlabelled lanes; Return clears every enclosing frame and loop
   `live` (LoopFrame gained `outer`); continuing runs under `live` after
   the body; BreakIf leaves like a break; refusals named (Discard,
   misplaced BreakIf, jumps/return in continuing). `ControlFlowCodegen/`
   (discardBelow golden), `ControlFlow/` (CPU, no allocation),
   `ControlFlowDevice/` (Metal and lavapipe; kernels cross-checked). Vulkan
   now enables `shaderDemoteToHelperInvocation` on 1.3, which glslang's
   SPIR-V 1.6 `discard` needs (validation found it). Unverified: FXC on
   switch with discard/return in a clause and the `while (true)` form, by
   documentation only. Not done: a `breakSwitch`, WGSL's mandatory default
   (optional here), body variables visible to the continuing block (C++
   scoping already prevents naming them).
9. **Types** — f16 as a value type (gated on a device query), non-square
   matrices, mutable private arrays, emitted structs and struct buffers.
   **Part A done** (matrices, private arrays, i32 buffers): six `ValueType`s
   and handles over `detail::MatrixHandle<C, R>` (`MatrixOf`, `ShaderMatrixLike`,
   `matrixColumns`/`matrixRows`/`transposedMatrix`, `hlslTypeName`);
   `float2x3(c0, c1)` …, `m[i]` (`ExprKind::MatrixColumn`; HLSL
   `transpose(m)[i]`), shape-changing `transpose`, M*v, v*M, M*M where shapes
   agree (templates beside the square overloads), +, -, scale. HLSL keeps the
   square convention: the value is the matrix itself in `floatRxC`, built as
   `transpose(floatCxR(cols))`, products `mul()` in source order. Uniforms:
   every shape, each column at a 16-byte stride (`uniformColumnStride`, API
   side scatter in `ShaderUploadVisitor`); Metal declares two-row ones as
   `float4 uN[C]` and rebuilds them from `.xy`; HLSL/std140 pad cursors from
   `uniformPackedSize` (HLSL's last column register holds only its rows) -
   `Float2x2`/`Float3x3` refusals lifted. Private arrays: array variables
   (`ShaderGraph::addArrayVariable`/`addVarElementRead`/`assignElement`, a
   `VarRead` with an index arg, an `Assign` with `index`), declared `T vN[N]`
   plus a fill loop, initial values as element assigns; Int/UInt index clamped
   as graph nodes (`min(max(i, 0), N-1)`, `min(i, N-1u)`; literals folded or
   asserted), so out-of-range is the end element everywhere; stage-attributed
   like `var`. i32 buffers: `IntInputBuffer`/`IntOutputBuffer` with the u32
   pair's reads/records/writes, render-readable, `packed_intN` on Metal, CPU
   `Bindings::set` over `int32_t` spans. CPU: matrix ops on columns x rows
   (`Node::rows/columns`), `MatrixColumn` and `VariableElementRead` ops, array
   variables sized in the workspace, per-lane scatter on element assign.
   `TypesCodegen/`, `Types/` (CPU, no allocation), `TypesDevice/` (Metal
   and lavapipe; every shape cross-checked exact, sort/histogram/clamp, i32,
   every shape as a uniform, fragment and vertex arrays, a render uniform).
   Unverified: HLSL by documentation (FXC dynamic indexing of
   `transpose(m)[i]` and of local arrays; cbuffer packing a scalar into a
   matrix's last register), D3D12 and Vulkan C++ unchanged. Left: whole-array assignment/copy and arrays as
   values, `arrayLength`.
   **Part B open**: f16 values and matrices (gated on a device query),
   emitted structs and struct buffers.
10. **Interpolation qualifiers** on varyings. **Open.**
11. **Override constants** — specialization at pipeline creation. **Open.**
12. **Emitted functions** — the largest change: the graph would need a
    function node rather than inlining. **Open.**
13. **Structured compile errors and real reflection**, and removing or
    filling `ShaderSource::bindings`. **Open.**

## Verification

- macOS: the Metal suites pass (`GPUCodegenTests`, `CpuComputeTests`,
  `GPUTests`, `GPUWidgetsTests`, `SpirvTests`, `UITests`, `MLGraphTests`;
  `HiddenWindow/orderedOutWindowStopsAndResumes` is a known flake), and the
  whole project builds as a unity build with its tests.
- Linux: the three CI steps pass on lavapipe (build, Weston ctest, Xvfb
  ctest), and the Weston step again under `EACP_VK_VALIDATION=1` with no
  new VUID (only the pre-existing `VUID-RuntimeSpirv-Workgroup-06530` in
  `SharedMemory/aKernelReportsWhatItDeclares`). Every `…Device/` suite runs
  there too, so the GLSL and Vulkan C++ of every stage have run; glslang
  compiles every emitted GLSL source inside the codegen suites.
- Windows: never built. No FXC or DXC on the Mac, so no emitted HLSL has
  been compiled and the D3D12 C++ is uncompiled; every "by documentation"
  note above stands until the Windows lanes (FXC, and the D3D12 suites on
  WARP) check it.
