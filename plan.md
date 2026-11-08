# Shader EDSL: gaps against WGSL

The C++ shader EDSL (`Lib/eacp/GPU/Codegen`) records a `ShaderGraph` and
emits MSL, HLSL and GLSL. Measured against WGSL, the WebGPU shading
language, it is missing the items below. Every one of them is reachable
today from a hand-written `ShaderSource` (`msl`/`hlsl`/`glsl`), so these
are gaps in what the EDSL can express, not in what the backends can run.

Status after each item: **done**, **partial** (a narrower form exists,
described) or **absent** (nothing). Paths are under `Lib/eacp/GPU/Codegen/`
unless given.

Where it stands: stages 1–12 of "Suggested order" are done - stage 9's part A
(non-square matrices, private arrays, i32 buffers) and both halves of part B
(f16 values, vectors and matrices, f16 storage buffers, the device query; then
emitted structs and buffers of them), stage 10's interpolation qualifiers
with a sample-rate-shading device query, stage 11's pipeline-overridable
constants (Metal function constants, Vulkan specialization constants, a D3D
compile per value set), and stage 12's emitted user functions (defined once
per source, MSL passing module-scope resources as parameters, the CPU executor
inlining at plan time), and stage 13's structured compile messages
(`ShaderLibrary::compilationInfo()`) and graph-derived reflection
(`ShaderReflection`, the dead `ShaderSource::bindings` removed). Every stage of
the suggested order is done. What each done stage
left behind is listed under it and in its section; "Verification" at the end
says what has and has not been run.

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
  `sampleRateShading` where the device has it; since stage 10
  `Device::supportsSampleRateShading()` reports it and `ShaderProgram::prepare`
  refuses a program reading `sampleIndex()` where it is absent.
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

`ValueType` (`ShaderTypes.h`) has bool, i32, u32, f32, f16 and their 2–4
vectors with every swizzle. Missing:

- `f16` as a value type — **done**: `Half`, `Half2/3/4` (`ValueType::Half`
  … after `Bool4`, so the old enum order is unchanged), gated on
  `Device::supportsShaderFloat16()`; see step 9. The packed float-buffer
  access (`packHalf2`/`unpackHalf2`, `readHalf*`, `writeHalf*`) is unchanged.
- Non-square matrices (`mat2x3` … `mat4x3`) — **done**: `Float2x3` …
  `Float4x3` (columns first; HLSL `floatRxC`), see step 9.
- `f16` matrices — **done**: `Half2x2` … `Half4x3`, every shape, over
  `detail::MatrixHandle<C, R, true>`; see step 9.
- Mutable fixed-size private arrays — **done**: `privateArray<T, N>()` /
  `privateArray(values...)`, see step 9. `ConstantArray` unchanged.
- Runtime-sized arrays of i32 — **done**: `IntInputBuffer`/
  `IntOutputBuffer`. Of f16 — **done**: `HalfInputBuffer`/
  `HalfOutputBuffer`. Of structs — **done**: `StructInputBuffer<T>` (every
  stage) / `StructOutputBuffer<T>` (kernels), WGSL's storage layout, see
  step 9.
- Emitted structs — **done**: a C++ struct of handles listed with
  `EACP_SHADER_STRUCT(Name, members...)` (`Codegen/ShaderStruct.h`) is a
  named type in all three dialects, declared once and only where used; struct
  `var`s, member assignment, private arrays of structs, nested structs. Left:
  fixed-size array members, struct varyings and uniforms (refused: pass the
  members), arrays of structs as values.
- Pointers and references — absent (no such `ExprKind`).
- `atomic<i32>` — **done**: `IntAtomicBuffer` (`atomic_int` /
  `RWStructuredBuffer<int>` / `int[]`), bound from `int32` (CPU
  `Bindings::set` takes `std::span<std::int32_t>`); `sharedAtomic<Int>`.

## 3. Address spaces and bindings

- Uniform buffers — partial: one packed `Uniform<T>` block per program;
  every matrix shape now crosses (step 9); `Bool` still refused by
  `static_assert` in `ShaderBuilder::uniform`, and so is every f16 type (there
  and in `Uniform<T>`): the block packs four bytes a component, FXC has no
  16-bit constant and std140 would need `uniformAndStorageBuffer16BitAccess`.
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
- Emitted user functions with parameters and return values — **done**
  (stage 12): `function<R(Args...)>(name, body)` (`Codegen/ShaderFunction.h`),
  defined once per source and called by name; `returnEarly(value)` from any
  statement. Plain C++ helpers still inline, unchanged. Not offered: pointer
  parameters, recursion (as WGSL), stage built-ins, shared memory, barriers,
  group folds and constant arrays inside a body (pass values as arguments).

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

**Done** (stage 11): `overrideConstant<T>(name[, default])` on `ShaderBuilder`,
`ComputeKernel`/`ComputeProgram` and `ShaderProgram`, T one of `Bool`, `Int`,
`UInt`, `Float`, `Half`; values by name in `OverrideValues`, given to
`prepare(..., values)` / `specialize(values)` or the CPU's
`PlanOptions::overrides`. Real specialisation on Metal (function constants)
and Vulkan (specialization constants); on D3D12 a compile per value set with
`EACP_OVERRIDE_<id>` macros. See step 11.

- Overrides as a workgroup size or a private array length — refused by design
  (documented): both are C++ constants of the graph, fixed before a pipeline.
- WGSL's numeric `@id` — not offered; the name is the key, the id the order of
  declaration.
- Override expressions (`override b = a * 2`) — no separate form: the handle
  is an ordinary value, so `a * 2.f` is that expression, folded by each
  compiler once specialised.

## 7. Interpolation qualifiers

**Done** (stage 10): `varying(value, Interpolation)`, WGSL's
`@interpolate(type, sampling)`; see step 10.

- Integer varyings are flat automatically (`flat` / `nointerpolation` /
  `[[flat]]`), and only flat: any other request asserts, as WGSL requires.
- User-chosen `flat` on float varyings — **done**: `Interpolation::flat()`
  (WGSL `first`) and `flat(Either)`, printed alike; every backend takes the
  first vertex.
- `linear` (no perspective) — **done**: `Interpolation::linear()`.
- `centroid`, `sample` — **done**, with `perspective` or `linear`; `Sample`
  gated on `Device::supportsSampleRateShading()`.
- Bool varyings — refused (unchanged).
- f16 varyings and vertex attributes — refused by `static_assert` (step 9):
  a 16-bit stage input/output needs Vulkan's `storageInputOutput16`, which
  most devices lack. Carry a `Float` and `toHalf` it. Unchanged by stage 10.

## 8. Compilation and reflection

**Done** (stage 13).

- Structured compile errors — **done**: `ShaderLibrary::compilationInfo()`,
  WebGPU's `GPUCompilationInfo` (`Shader/CompilationInfo.h`): messages with a
  type (error/warning/info), text, line, column, FXC's span length and the
  stage that compiled it; warnings kept on a success where the compiler gives
  them (not Metal, which drops them), every invalid library holding an error,
  `compilationInfo(constants)` for a D3D value-set variant. Portable parsers per
  compiler output. The log line stays; `Dxc::status().error` is unchanged (the
  loader's state, not a compile's).
- Reflection — **done**: `ShaderReflection` (`Codegen/ShaderReflection.h`)
  derived from the graph, on `GeneratedShader::reflection`,
  `ShaderProgram::reflection()` and `ComputeKernel::reflection()`.
  `ShaderSource::bindings`, `withBinding`, `ResourceBinding` and `ResourceKind`
  are removed. Hand-written sources get no reflection (see step 13).

## Housekeeping found during the audit

- `ShaderValue.h` has a comment saying `GPU::Texture` has no mips yet;
  mipmaps exist. **Done.**
- `CommandBuffer/CopyPlan.cpp` refuses copies with depth and says eacp
  lacks 3D textures; `TextureType::ThreeD` exists. **Done** (wording only;
  a buffer-texture copy with depth is still refused).
- `ShaderSource::bindings` / `withBinding` / `ResourceBinding` are unused.
  **Done** (stage 13): removed, with `ResourceKind`; the EDSL's reflection is
  read from the graph instead.

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
   **Part B, first half done** (f16): `Half`, `Half2/3/4` (swizzles through a
   `Halves` family), `Half2x2` … `Half4x3`, `HalfInputBuffer`/
   `HalfOutputBuffer`. Thirteen `ValueType`s after `Bool4` (`lastValueType`,
   `isHalf`, `halfVectorOf`, `matrixOf(c, r, half)`, `matrixColumnType`,
   `halfEquivalent`/`floatEquivalent`); `baseOf` takes the f16 handles, so
   every float intrinsic and operator template takes them, and the families
   never mix: `ShaderScalar<T>`, `ScalarBeside<S, T>` and `SameShaderShape`
   keep a `Float` off a `Half` (vector-scalar operators, `ShapedBeside`,
   scalar comparisons, matrix scale per family); `length`/`dot`/`distance`
   return the family's scalar, `cross` has a `Half3` overload, the
   literal-`select` returns `ShaderBase<T>`. A float literal beside a Half is
   an f16 constant (`detail::literalNodeBeside`, `ShaderGraph::
   addHalfConstant`, held rounded through `halfFromFloat`), as are
   `halfConstant(x)`, `Var<Half> = x`, `privateArray<Half>().set(i, x)`,
   `half2/3/4(...)` literals. WGSL's f32-only built-ins - the derivatives,
   `quantizeToF16` - and eacp's float-written `erf`/`erfc`/
   `saturatingTanh`/`log10` take `FloatValueLike` only. Conversions:
   `toHalf` from Float/Int/UInt/Bool (scalars and vectors), `toFloat`/`toInt`/
   `toUInt` from Half (call nodes named by `typeName`). Matrices:
   `MatrixHandle<C, R, IsHalf>` (`isHalf`, `MatrixOfFamily`, `HalfMatrixOf`,
   `VectorOfFamily`), `half2x3(c0, c1)` …, template products for every half
   shape (squares included), `+`/`-` per family, `transpose`/`determinant`
   templates. Spellings: MSL `half`/`halfN`/`halfCxR`, literals `0.5h`,
   `packed_halfN` vector loads, `device [const] half*` buffers, and half
   overloads of the `eacpAsinh` helper (Metal converts no vector implicitly);
   GLSL `float16_t`/`f16vecN`/`f16matCxR`, `0.5hf`, `#extension
   GL_EXT_shader_explicit_arithmetic_types_float16` and, for a half buffer,
   `GL_EXT_shader_16bit_storage` - both only where `ShaderGraph::usesHalf()`/
   `usesHalfStorage()`, so f16-free sources are byte-identical; HLSL holds f16
   in `float` (`hlslTypeName`, `hlslTypeNameFor` for conversions, matrices
   `floatRxC`) and wraps every f16 Binary/Mul/Call result but the exact calls
   (abs/min/max/clamp/floor/ceil/trunc/round/sign/transpose) in
   `eacpRoundHalf` (`isRoundedToHalf`; one `f16tof32(f32tof16(x))` overload
   per rounded type, a matrix one per row), and an f16 buffer is
   `[RW]StructuredBuffer<uint>` words: element i read as `f16tof32(word >>
   16 * (i & 1))`, written as `InterlockedAnd` then `InterlockedOr` on the word
   so the neighbouring thread's half survives (offsets and sizes whole words on
   D3D). Why that mapping: FXC SM5 has no 16-bit storage or arithmetic
   (`half` is `float`, `min16float` a precision hint with no defined rounding),
   so rounding every result is the only way HLSL computes WGSL's values; it
   costs ALU instead of saving it, documented. Gating: `Device::
   supportsShaderFloat16()` - Metal and D3D12 true on a valid device; Vulkan
   probes `shaderFloat16` + `storageBuffer16BitAccess` (1.2/1.1 features on the
   core floor, `VK_KHR_shader_float16_int8` + the 1.1 16-bit storage struct
   below it) and enables them at device creation. `ComputeProgram::
   fitsShaderFloat16`/`ShaderProgram::fitsShaderFloat16` and `prepare()`
   refuse an f16 program on a device without it (logged, invalid pipeline),
   like storage textures. Render: a fragment may compute in f16 and read a
   `HalfInputBuffer` (bound like `IntInputBuffer`). Refused by `static_assert`:
   f16 uniforms (`ShaderBuilder::uniform`, `Uniform<T>`), varyings, vertex and
   instance inputs, `Shared<Half>`; group reductions and the SIMD matrix stay
   f32 (overloads only). CPU: f16 lanes are floats; `isFloatFamily` takes the
   half vectors, `Plan::Node::roundsToHalf` on every non-leaf f16 node and
   `evaluateRange` rounds it (`quantizeToF16`, nearest-even) after evaluating -
   HLSL's semantics, the correctly rounded half for + - * / sqrt; half slots
   are `SlotView::half` (two-byte elements, widened on load, narrowed on
   store, ramp shortcuts skipped), `Bindings::set` over `std::uint16_t` spans,
   a store's family checked against its slot's. `Float16Codegen/` (10 cases:
   full three-dialect goldens of a small kernel, every built-in, every matrix
   shape, vars/private/constant arrays, buffers, extensions only where used,
   a fragment, and compile-time checks that the families never mix),
   `Float16/` (CPU, 8 cases: every built-in and every matrix shape equal to
   the f32 kernel rounded, literal/conversion rounding and ties, a running sum
   rounded per step, records, out-of-range, binding refusals, no allocation),
   `Float16Device/` (9 cases, Metal and lavapipe, cross-checked: exact
   arithmetic/conversions, every matrix shape exact over quarters, built-ins
   within 16 f16 ulps of max(|result|, 1/4), shared words between threads, a
   per-step sum, a fragment in f16; GPU halves self-skip without f16;
   `CpuCrossCheck.h` gained f16 arrays and that skip). Unverified: HLSL by
   documentation (FXC `f32tof16` on vectors, `InterlockedAnd/Or` on a root
   UAV `RWStructuredBuffer<uint>` element), D3D12 C++ uncompiled. Left: f16 in
   uniforms, varyings, workgroup memory, group reductions and SIMD matrices;
   matrix conversions (`toHalf(Float2x3)`); `write2/3/4` vector stores and
   atomics on half buffers; bitcasts to/from f16; an f16 literal past 65504
   is an infinity, which no dialect has a literal for (the f32 literals have
   the same gap). **Part B, second half done** (structs): `EACP_SHADER_STRUCT(
   Name, members...)` inside a C++ struct of handles (scalars, vectors and
   matrices of every family, `Bool`, nested such structs) adds the shader's
   view of it through member pointers, on the `EACP_SHADER` macro engine (moved
   to `Codegen/MemberList.h` so both share it). `StructLayout<T>` is WGSL's
   storage layout at compile time (`members`, `offsetOf<&T::m>()`, `size`,
   `alignment`, `stride`, `fieldCount`, `hostShareable`), on `storageAlignment`
   / `storageSize` / `storageColumnStride` / `storageComponentOffset` in
   `ShaderTypes.h`; `StructBufferData<T>` fills and reads host bytes by member
   pointer (one level of nesting by two pointers). Graph: `ValueType::Struct`
   past `lastValueType` (`isHalf` now bounded above); `ShaderGraph::addStruct`
   declares a `StructType` (members with offsets, nested ids first) and fills
   its `fields` - every member at every depth, pre-order, with path, offset,
   owner and a run of flat `components` (byte offset, f16 or not). A struct
   value is a `Construct` of type Struct (`Expr::structType`); fields are read
   as a `VarRead` or `BufferRead` with `Expr::member` (leaves only - the C++
   value is its leaf handles, so an element read loads only what is used, and
   read-only fields share through the read cache, now keyed on the member) and
   written as an `Assign` or `Store` with `Statement::member` (-1 whole, or a
   nested struct whole). Struct variables and private arrays
   (`addStructVariable`, `addStructArrayVariable`, `clampVariableIndex` once per
   element access), struct slots (`addStructBuffer`, `addBufferMemberRead`,
   `addMemberStore`), `addStructZero`, `fitsField` asserts. API: `Var<T>` and
   `PrivateArray<T, N>` partial specialisations for structs (`get()`,
   `operator=`, `set(&T::m, v)`, `set(i, v)`, `set(i, &T::m, v)`),
   `ShaderBuilder::structInputBuffer<T>()` / `structOutputBuffer<T>()`,
   `write(buffer, i, value)` / `write(buffer, i, &T::m, value)`,
   `structZero<T>()`, `var(struct)`; `Uniform<StructInputBuffer<T>>` /
   `Uniform<StructOutputBuffer<T>>` members through one `onStructBuffer`
   visitor hook that carries a pointer to `detail::declareStruct<T>`; the
   compute bind visitor binds them as inputs/outputs, the render one to both
   stages (read-only). Emission (`ShaderStructEmission.{h,cpp}`): declarations
   in the preamble after the dialect header, in declaration order, nothing
   when no struct is used, so every existing golden is byte-identical. GLSL:
   the struct as is, `Name(...)` constructs, an std430 block of `Name` (its
   rules are WGSL's). MSL: a shareable struct laid out by hand - a
   three-vector as `packed_` (12 bytes, align 4), `uchar eacpPadN[k]` in every
   gap and at the tail, every other type (matrices included, f32 and f16) is
   already WGSL's size with an alignment no larger - so `device [const] T*`
   strides right; a packed field read is wrapped back to its vector;
   `eacpMake<Name>(...)` builds a value from a zeroed one. HLSL: a plain value
   struct (f16 as float, matrices rows-first) plus `eacpMake<Name>`, and the
   buffer a `[RW]ByteAddressBuffer` at the offsets: `asfloat/asint(LoadN)`,
   matrices column by column through `transpose(floatCxR(...))`, f16 out of
   its word (`f16tof32(Load(word) >> 16)`, the word and shift computed at run
   time only where the stride is not a multiple of 4), stores `StoreN(asuint)`
   per leaf (a matrix per column of its transpose) and f16 as `InterlockedAnd`
   then `InterlockedOr` on the word; a value or index the store prints more
   than once is named first. Why not `StructuredBuffer<T>`: D3D packs it with
   4-byte alignment only (a vector after a scalar moves, matrix columns pack
   together) and SM5 has no 16-bit member. CPU: a struct value is its flat
   components laid end to end in lanes (`Construct` concatenates; a node's
   components from the struct, at most 255), a struct variable is a variable of
   that many components (a field read aliases the storage at its first
   component, an element read is `VariableElementRead` with
   `Node::component`; an assign writes `Step::component` on), a struct slot
   carries its stride and component table (`Plan::structStride`,
   `structComponents`; `SlotView::loadStruct`/`storeStruct`, f16 widened and
   narrowed) and is read by `Op::StructRead`, written field by field from the
   value every lane evaluated first; `Bindings::set` takes the bytes or host
   records of the stride's size and a dispatch refuses any other stride or
   element type; out of range reads zero and stores nothing. Also fixed: the
   CPU scratch pool's free lists were indexed by component count up to 16,
   which a struct value overran. `StructCodegen/` (8 cases: full three-dialect
   goldens of a small kernel, the layout of vec3-then-scalar, scalar-then-vec3,
   matrices, f16 members, nested structs and an f16-only struct as
   `static_assert`s and graph fields/components, each dialect's layout of a
   nested struct with f16 and matrix members, declared once and only where
   used, vars and private arrays and a Bool struct, f16 fields' byte
   addressing, a fragment, every graph planned by the CPU executor), `Struct/`
   (CPU, 8 cases: offsets, read and write at the layout with padding untouched,
   nested/matrix/f16 fields and a whole write that swaps fields, an f16-only
   struct at a 2-byte stride, vars and private arrays, out of range, binding
   refusals and host records, no allocation), `StructDevice/` (6 cases:
   cross-checked field for field against the CPU - `CpuCrossCheck.h` gained
   struct inputs/outputs, `Readback::structs`, a field-wise agreement - a
   particle kernel, every value type (17 members, 464 bytes, every matrix
   shape), nested structs and f16 written whole from themselves, an f16-only
   struct whose elements share words between threads, vars and private arrays,
   a fragment reading a struct buffer). Unverified: HLSL by documentation only
   (FXC `ByteAddressBuffer`/`RWByteAddressBuffer` `LoadN`/`StoreN` and the
   two-argument `InterlockedAnd`/`InterlockedOr` methods, a root SRV/UAV over a
   raw view of the same buffer - the D3D12 C++ is unchanged), MSL's padding
   rule beyond what the device suite exercises on one Apple GPU. Left:
   fixed-size array members (each element would need an array value and a
   dynamic index through all three dialects and the CPU), struct varyings and
   uniforms, arrays of structs as values, a member write naming a field below
   the top level (write its nested struct whole instead), `StructBufferData`
   members more than one struct deep.
10. **Interpolation qualifiers** on varyings. **Done**: `Interpolation`
    (`ShaderGraph.h`) is a type - `InterpolationType::Perspective`/`Linear`/
    `Flat` - and a sampling - `InterpolationSampling::Center`/`Centroid`/
    `Sample`/`First`/`Either` - built as `Interpolation::perspective(s =
    Center)`, `linear(s = Center)`, `flat(s = First)`; `ShaderBuilder::
    varying(value, interpolation)` beside the unchanged `varying(value)`, which
    takes `defaultInterpolation(type)` (perspective at the centre, flat/first for
    Int/UInt); `ShaderProgram::varying` has the five two-argument overloads.
    `ShaderGraph::addVarying` asserts `isValidInterpolation` (flat only with
    First/Either, the others only with Center/Centroid/Sample, integers only
    flat) and keeps it on `VaryingSlot::interpolation`; attributes the fragment
    reads directly (the emitter's implicit varyings) take the default for their
    type. Spellings (`ShaderDialect.h`): MSL after the member -
    `[[centroid_perspective]]`, `[[sample_perspective]]`,
    `[[center_no_perspective]]`, `[[centroid_no_perspective]]`,
    `[[sample_no_perspective]]`, `[[flat]]`; HLSL and GLSL ahead of it, the
    type's then the sampling's - `noperspective`/`nointerpolation`/`flat` then
    `centroid`/`sample`, so `noperspective centroid`; perspective at the centre
    prints nothing in all three (asked or not), so every existing golden is
    byte-identical. Provoking vertex: Vulkan's default is the first vertex
    (eacp never enables `VK_EXT_provoking_vertex`), D3D's leading vertex is the
    first, and Metal's is the first - measured: vertex `3i` of a list, vertex
    `i` of a strip's triangle `i`, odd triangles included. So WGSL's `first` is
    guaranteed and `Either` is printed as `First`. `Sample` runs the fragment
    stage per sample: `ShaderGraph::usesSampleRateShading()` (a `Sample`
    varying or `sampleIndex()`), `Device::supportsSampleRateShading()` (Metal
    and D3D12 true on a valid device, Vulkan the `sampleRateShading` feature
    enabled since stage 2, `VulkanShared::supportsSampleRateShading`),
    `ShaderProgram::fitsSampleRateShading`, and `prepare()` refuses a program
    that does not fit (logged, empty library, invalid pipeline), as it does f16.
    CPU: nothing - the interpreter is compute-only. `InterpolationCodegen/` (6
    cases: full three-dialect goldens of every qualifier beside unasked ones and
    two integer varyings, what the graph records, asking for perspective prints
    exactly the unasked source, each qualifier alone on both GLSL sides, a
    carried attribute at its default, which graphs run per sample; validity and
    MSL spelling tables as `static_assert`s), `InterpolationDevice/` (6 cases: a
    quad receding from w = 1 to w = 4 where linear gives t and perspective
    (t/4)/((1 - t) + t/4) at every pixel centre, unasked equal to perspective;
    four triangles with a distinct value per vertex where every pixel holds
    exactly its triangle's first vertex's value under flat and flat-either and
    an unasked `vertexId()` varying is 0/3/6/9; a strip whose two triangles give
    vertices 0 and 1; a four-sample triangle whose hypotenuse leaves cut pixels'
    centres 0.3 px outside, where centre interpolation extrapolates a
    barycentric below zero and perspective and linear centroid never do; the
    device query; the pixel coordinate's sub-pixel offset zero at every centre
    under centre interpolation, non-zero in every pixel under sample, zero
    under centroid off the quad's diagonal, and the triangle never
    extrapolating under sample; the per-sample case self-skips without the
    query). Unverified: HLSL by documentation only (FXC's `noperspective
    centroid`/`sample` modifiers on a `VertexOut` member used as both the
    vs_5_0 output and the ps_5_0 input, D3D's leading-vertex rule for strips);
    `Device-Windows.cpp` uncompiled. Left: interpolation on struct varyings
    (struct varyings are still refused), WGSL's `@interpolate` on a fragment
    built-in (none takes one), `VK_EXT_provoking_vertex`'s last-vertex mode (no
    WGSL spelling).
11. **Override constants** — specialization at pipeline creation. **Done**:
    `ExprKind::Override` (index = id), `ShaderGraph::addOverride(name, type[,
    defaultBits])` declaring an `OverrideDeclaration` (name, type, default as
    `PipelineConstant::bits`, node) - a name declared again is the same node
    (asserted to agree), names asserted identifiers, types the five scalars;
    `findOverride`, `overrides()`. `ShaderBuilder::overrideConstant<T>(name)`
    and `(name, default)` over `detail::OverrideDefault<T>` (float for Float
    and Half - rounded to the nearest half - int, unsigned, bool;
    `OverridableHandle`), forwarded by `ComputeKernel` and `ShaderProgram`.
    Backend layer (`Shader/PipelineConstants.{h,cpp}`, in eacp-gpu-codegen):
    `PipelineConstantType`, `PipelineConstant` (id, type, 32 bits; `of*`/`as*`,
    `asHalfBits`), `PipelineConstants` (id-ordered, `set` replaces, `==`),
    `hlslOverrideMacro`/`hlslLiteralOf`/`hlslDefinesFor`;
    `ShaderSource::withPipelineConstants()`/`hasPipelineConstants` (set by
    `buildShader` for a graph with an override), `ShaderLibrary::
    hasPipelineConstants()` and `nativeLibrary(constants)`,
    `ComputePipeline(device, library, constants)` and `Device::
    makeComputePipeline(library, constants)`, `RenderPipelineDescriptor::
    constants`. EDSL layer (`Codegen/OverrideValues.{h,cpp}`): `OverrideValues`
    (`set(name, float|double|int|unsigned|bool)`, `const char*` deleted, a name
    set again replaced) and `resolveOverrides(graph, values)` ->
    `ResolvedOverrides{values (every override by id, defaults filled),
    specialized (only those not equal to their default, bitwise), error}`:
    unknown name, wrong type, no default and no value, a non-finite float, a
    Half past f16 refused naming the override; int<->unsigned accepted where
    lossless. Emission (`Codegen/ShaderOverrideEmission.{h,cpp}`, in the
    preamble after the dialect header, nothing without an override so every
    golden is byte-identical; the body reads `override_<name>`): MSL `constant
    T eacpOverrideN [[function_constant(N)]];` then `constant T override_name =
    is_function_constant_defined(eacpOverrideN) ? eacpOverrideN : default;`
    (no default: the function constant is `override_name` itself); GLSL
    `layout(constant_id = N) const T override_name = default;` (no default:
    the type's zero; f16 as `float16_t`/`hf` with the f16 extension, which
    `usesHalf()` already adds - glslang takes it, SPIR-V 1.6 and 1.3, and
    `(-2147483647 - 1)` as an initialiser); HLSL `#ifndef EACP_OVERRIDE_N` /
    `#define EACP_OVERRIDE_N default` / `#endif` / `static const T
    override_name = EACP_OVERRIDE_N;` (f16 the rounded value in `float`).
    Backends are given only `specialized`, so the shader's own defaults cover
    the rest and the empty set is the library as compiled. Metal:
    `Pipeline/MetalFunctions-Apple.{h,mm}` `Detail::makeMetalFunction` - for a
    library with pipeline constants `newFunctionWithName:constantValues:error:`
    (through `buildRetryingTransientFailures`) with an
    `MTLFunctionConstantValues` (Bool as `bool`, Half as its binary16), always,
    even for the empty set, since Metal hands out no pipeline-ready function of
    one without; plain `newFunctionWithName:` otherwise, so nothing else
    changes. Vulkan: `Pipeline/VulkanSpecialization-Linux.h` (a map entry per
    constant, a four-byte slot each, `VkBool32` for Bool, a 2-byte entry for
    Half; null for the empty set) on the compute stage and both render stages;
    the shared `VkPipelineCache` keys on the specialization data itself, so
    nothing changed there. D3D12 - the choice: compile per value set, not a
    hidden cbuffer. `ShaderLibrary-Windows` keeps its `ShaderSource` when it
    has pipeline constants and `nativeLibrary(constants)` compiles a variant
    per distinct non-empty set on first use, FXC with `D3D_SHADER_MACRO`s or
    DXC with `-D NAME=VALUE` (`Dxc::Detail::compile` gained `defines`), cached
    for the library's life under a mutex (`OwnedVector<D3D12ProgramVariant>`);
    the empty set is the base program, precompiled or not. Cost: a full compile
    of every stage per distinct set, once, on the pipeline-building thread, and
    each variant's bytecode held; nothing per draw. A cbuffer would cost a root
    slot, a bind per draw and an unfoldable value. Consequence: precompiled
    DXBC covers the default set only, and other sets compile at run time even
    under `EACP_REQUIRE_PRECOMPILED_SHADERS=1`. Programs: `ComputeProgram::
    prepare(device, values)` / `prepare(values)` and `ShaderProgram::
    prepare(descriptor, values)` (the old overloads pass empty values) compile
    the library, forget every cached pipeline and specialise; `specialize(
    values)` makes the pipeline for a set current - the cached one when the
    resolved `specialized` set is equal (so a default given = left out), else a
    new one on the same library (`OwnedVector<Specialization>`, stable
    addresses, `pipeline()` points at the current one). Refused values log and
    make an invalid pipeline current (a separate `refusedLibrary`/
    `refusedPipeline`, so later good values still work); `specialize` before
    `prepare`, or after a device refusal, returns false. `prepare` still always
    rebuilds, which is what a device restore needs - the cache only spans
    `specialize` calls. Precompiled: unchanged code path - the metallib and
    SPIR-V hold the unspecialised functions, the pipeline specialises at load.
    CPU: `PlanOptions::overrides`, resolved in `PlanBuilder::build` (refusal =
    plan failure with the reason, logged once at plan creation); an Override
    node is a `ConstantNode` with the resolved word (Bool a mask, Half the
    rounded float), dispatch-level and lane-invariant (`constantForm`, so ramp
    detection sees through `id * stride`); nothing at dispatch.
    `OverrideCodegen/` (11 cases: full three-dialect goldens of a kernel with
    each type and a no-default override, both render stages, the f16
    spellings, no declarations and no `hasPipelineConstants` without one, a
    name declared again, resolution by name and type, a default given = left
    out, every refusal named, HLSL defines, `INT_MIN` compiling, CPU plans),
    `Override/` (CPU, 5 cases: defaults, two value sets on two executors,
    integers keeping every bit, every refusal leaving no plan, no allocation),
    `OverrideDevice/` (7 cases: a kernel with two value sets and with defaults
    cross-checked against the CPU - `CrossCheck::overriding(values)` -, one
    library keeping a pipeline per set with pointer identity on repeats and on
    a default given, refusals then good values, a render program whose vertex
    stage scales and whose fragment stage colours from overrides under two sets,
    an f16 override over three values, a precompiled kernel loaded under the
    require flag and specialised with two sets). Not offered (documented in
    the README "Overrides"): overrides sizing a workgroup or a private array,
    numeric `@id`. Unverified: all HLSL and the D3D12 C++ (uncompiled), as
    for every stage.
12. **Emitted functions** — the largest change: the graph would need a
    function node rather than inlining. **Done**: WGSL's `fn` as
    `function<Signature>(name, body)` on `ShaderBuilder`, `ComputeKernel`/
    `ComputeProgram` and `ShaderProgram` (`Codegen/ShaderFunction.h`):
    `auto f = function<Float(Float, Float2)>("f", [&](Float a, Float2 b) {
    ...; return v; }); f(x, y);`. The body runs once with a handle per
    parameter; `returnEarly(value)` returns from any statement (if, loop,
    switch clause), `returnEarly()` leaves a void function. Types: scalars,
    vectors, matrices of every family, f16 and shader structs, by value; void.
    Graph: `FunctionDeclaration` (name, parameters, result type/struct, root
    block, callees, `writesStorage`/`readsMutableStorage`/`discards`/`isPure`
    with callees folded in), `beginFunction`/`addParameter`/`endFunction`
    (swaps the open-block stack for the function's root, so jumps never reach
    a caller's loop; restores it and the vertex-stage flag), `addReturn(value)`,
    `addFunctionCall`, `ExprKind::FunctionCall` (index = function, args), and
    `StatementKind::Call` (value = the call node, slot = the result variable or
    -1). Parameters are variables owned by the function (`variableFunction`),
    declared by the signature, so every VarRead/field-read path prints and
    plans them unchanged; a struct parameter is a struct variable. Decisions:
    (1) resource access - a body may read uniforms, overrides, buffers,
    textures and samplers and store to buffers/textures and run buffer
    atomics; on MSL, where those are entry-point parameters, the function takes
    the ones its body reaches (callees included) as trailing parameters under
    the entry point's own names (`uniforms`, `textureN`/`samplerN`, `bufferN`,
    the buffer's access as the kernel declared it, read-only in a render
    program) and every call passes them (Naga's approach); HLSL/GLSL reach them
    as globals. (2) refused in a body, asserted at `endFunction`: stage
    built-ins, vertex inputs, varyings, thread/local/group ids, grid extent,
    shared memory and SIMD-group index, constant arrays (declared in the entry
    body), barriers, group folds, SIMD-group matrices, shared atomics, reads or
    writes of a caller's variable, assigning a parameter - WGSL makes the
    stage values entry-point parameters too, so they are passed as arguments.
    (3) stage checks by call site, transitively: the vertex stage may not call
    a function that stores, runs an atomic or discards (asserted in
    `addFunctionCall`) or takes derivatives (`vertexUsesImplicitDerivatives`
    looks inside called functions); a kernel none that discards (existing
    assert) or takes derivatives (new assert in `emitCompute`); the fragment
    anything. (4) expression or statement: a value function returning a
    non-struct that neither touches storage a kernel may write nor discards is
    an expression node, pure where its body and arguments are, printed where
    read (a local when read twice, re-evaluated per pass in a loop condition),
    callable from the vertex position without `vertexStage()`; every other
    call is a `Call` statement made where it is written (`T vN = fn_f(...)` /
    `fn_f(...);`), belonging to the stage open - so the staleness analysis
    sees a call's buffer writes (`collectCalledBufferWrites`) and its order is
    the program's. No recursion: a function is callable once complete, so not
    from its own body; no nested declarations; names are identifiers, unique.
    Emission (`Codegen/ShaderFunctionEmission.{h,cpp}`): `findCalledFunctions`
    walks the roots and each reached body (callees first, post-order), so a
    function is defined once per source, only where called, ahead of its
    callers, as `fn_<name>`; MSL/HLSL emit the union of both render stages
    once after the bindings, GLSL each stage's inside its `#ifdef`, a kernel
    after its globals and before the entry point. Bodies go through a fresh
    `StageEmitter` (parameters `markInScope`), a value `Return` prints `return
    v;`. Analyses over a stage (uniform reads, vertex texture slots, Metal
    buffer parameters, derivatives) take `appendFunctionBodyRoots`, and the
    stage-variable check skips function variables. Nothing changes where no
    function is called: every existing golden is byte-identical and a
    declared-but-unused function leaves the source as it was (tested). CPU:
    `ShaderGraph::withFunctionsInlined()` (`Codegen/ShaderFunctionInlining.cpp`,
    a `FunctionInliner` friend) copies the graph and replaces every call with
    a `Call` statement whose `body` is a fresh copy of the function's blocks,
    led by parameter Declares from the arguments, each `Return value` becoming
    an Assign of the call's result variable then a Return; an expression call
    is inlined in front of each statement reading it (per statement, so two
    calls in one expression keep two result variables and a value re-read
    after a write is recomputed), a loop whose condition calls one becomes
    `loop(true)` with the calls and `if (!cond) break` at the top of its body;
    a call in a constant array's element is refused by name. The plan builds a
    `Call` step's body with loop depth, continuing and switch context reset,
    and `runCall` (Statements.cpp) evaluates the step's hoisted nodes then runs
    the body in a `MaskFrame` with no parent and no loop, so a return ends its
    lanes for the body alone and the existing frame/`bodiesJumpOut` machinery
    does the rest. Function variables are shared across copies (no recursion,
    each copy re-declares them); result variables are per call. All at plan
    time; `Plan::Plan` inlines only where `functionCount() > 0`, through a new
    private `Plan::build`. Found while testing: the plan hoists a pure value
    two inlined bodies share to the `Call` step, which therefore has to
    evaluate its schedule. `FunctionCodegen/` (14 cases: full three-dialect
    goldens of a kernel with an early-returning function called twice by
    another and of a render program whose function both stages call and whose
    fragment function reads a uniform and a texture; a function called six
    times defined once; callees ahead of callers; returns from a loop, a switch
    clause and an if; storage-writing and output-reading functions as
    statements with MSL's buffer parameters; struct, f16 and matrix parameters
    and results; an unused function leaving the source byte-identical; purity;
    each GLSL stage defining what it calls; derivatives following the calling
    stage; a texture-writing function reading an override; a discarding
    fragment function; a struct-returning call in `vertexStage()`), `Function/`
    (CPU, 8 cases: early return, returns from loop and switch with calls in a
    loop condition, an if and a switch selector, calls in a continuing block
    and its break-if, the constant-array refusal, two calls in one expression
    and a call of a call, void functions storing and counting with an
    output-reading call, structs/f16/matrices by value, no allocation),
    `FunctionDevice/` (7 cases, Metal and lavapipe: five kernels cross-checked
    against the CPU - nested early returns, calls in conditions and break-if,
    void store/atomic plus output read, struct and matrix, f16 (GPU half skips
    without f16) - and two render programs: one function called from both
    stages with a uniform read through a nested call, a struct-returning call
    in `vertexStage()` beside a discarding fragment function). Unverified: all
    HLSL (FXC on functions taking `float2x2`, `discard` in a helper, a
    `switch` clause ending in `return`, functions over `RW*` globals) - by
    documentation only. Not done: pointer parameters (`ptr<function, T>`),
    stage built-ins and shared memory inside a body, workgroup barriers/folds
    in functions, calls in a constant array on the CPU, an expression form for
    struct results (no field-of-expression node), functions declared inside
    functions.
13. **Structured compile errors and real reflection**, and removing or
    filling `ShaderSource::bindings`. **Done.** Compile messages
    (`Shader/CompilationInfo.{h,cpp}`, in eacp-gpu-codegen):
    `CompilationMessageType` (Error/Warning/Info), `CompilationMessage` (type,
    message, 1-based line and column - 0 where none - FXC's span `length`,
    `std::optional<ShaderStage> stage`, `describe()`), `CompilationInfo`
    (`messages`, `hasErrors`, `countOf`, `add`, `append`, `describe`). Four
    portable parsers over a `CompileLog{text, stage, failed}`: `parseMetalLog`
    and `parseDxcLog` (clang's `file:line[:col]: severity: text`, read from the
    right so a drive letter's colon stays in the file, the earliest severity
    marker winning), `parseGlslangLog` (`ERROR|WARNING|NOTE|INTERNAL ERROR|...:
    0:line[:col]: text`, the "N compilation errors" summary dropped, a link
    error kept without a position), `parseFxcLog` (`file(line,col[-end]):
    error|warning Xnnnn: text` or `error Xnnnn: text`, the code kept at the head
    of the text, `length` from the span); every one drops excerpt and caret
    lines, and a log none of whose lines parses is one message of the trimmed
    text with no position, an error beside a failure and a warning beside a
    success. `ShaderLibrary::compilationInfo()` (filled through the `Native`
    constructor from a `CompilationInfo&` member declared before `impl`) and
    `compilationInfo(constants)`. Metal: the `NSError` text captured inside the
    `buildRetryingTransientFailures` lambda (on success too), parsed with no
    stage. Vulkan: each stage's glslang log, parsed as that stage's. D3D12:
    FXC's error blob (now on success as well) and DXC's `DXC_OUT_ERRORS`, each
    stage's, `Dxc::Detail::compile` taking the stage and a `CompilationInfo&`;
    each value-set variant keeps its own (`D3D12ProgramVariant::compilation`,
    `variantFor`), which `compilationInfo(constants)` returns, compiling the
    variant if no pipeline has. Invariant: an invalid library holds an error -
    the compiler's, or the library's own (no device, an empty source, a
    precompiled binary required and missing - `Precompiled::fallBackAfterMiss`/
    `fallBackAfterLoadFailure` now take the `CompilationInfo&` and add the same
    text they log - a module the driver refused, a compiler that failed
    silently, unsigned DXIL). Two changes under it in `eacp-spirv`: glslang is
    asked for columns (`EShMsgDisplayErrorColumn`, log text only - SPIR-V
    unchanged), and `compileGlsl` now returns the info log of a *successful*
    parse and link, which it used to drop, so glslang's warnings reach the log
    (nothing in the tree treated a non-empty log as failure; the precompile tool
    already prints a log beside a success as a note). Found: Metal on macOS 27
    returns no `NSError` for a library that builds with warnings (`#warning`,
    sync and async APIs alike) - warnings come only beside an error; documented,
    and the device test asserts only that whatever Metal reports there is a
    warning. Reflection - decision: fill from the graph, on `GeneratedShader`/
    the programs, and remove the dead plumbing. `ShaderSource::bindings`,
    `withBinding`, `ResourceBinding` and `ResourceKind` are gone (nothing filled
    or read them; a list beside the source would be a second account to keep in
    step, and the graph says more than it could); hand-written sources get no
    reflection (Vulkan's SPIR-V descriptor reflection stays internal - exposing
    it would cover one backend's compute layouts only). `Codegen/
    ShaderReflection.{h,cpp}`: `reflectShader(graph)` -> `ShaderReflection`
    with `bindings` (`ShaderBinding`: `BindingKind` UniformBlock /
    StorageBuffer / Texture / StorageTexture / Sampler / ComparisonSampler,
    slot, member name, `StageVisibility` - the render stages whose nodes reach
    it, from new `vertexStageNodes`/`fragmentStageNodes` in `ShaderEmitter.h`,
    every kernel binding compute - buffer access/element type/struct, texture
    kind/sampling/format/access), `uniformMembers` (name, type, offset, size;
    a kernel's grid extent members marked `gridExtent`) and
    `uniformBlockSize`, `vertexInputs`, `varyings` (from new `emittedVaryings`,
    the implicit carried inputs included with `attribute`), `overrides` (id,
    name, type, default as a `PipelineConstant`), `functions` (parameters,
    result, `calledFrom` stages - transitively, empty when never called -
    statement/storage/discard/purity), `structs`, `dispatchRank`,
    `workgroupSize`, `threadgroupMemoryBytes`, `usesHalf`,
    `usesSampleRateShading`; `findBinding(kind, slot)`/`(name)`,
    `findOverride`, `findFunction`. Names: `ShaderGraph::nameUniform`/
    `nameStorageBuffer`/`nameTexture` (lazy parallel string vectors, read by
    nothing emitted), set by `ShaderBuildVisitor` from the `EACP_SHADER` member
    names. One source of truth: `GeneratedShader` lost its `dispatchRank`,
    `vertexReadsUniforms`, `fragmentReadsUniforms` and `vertexTextureSlots`
    fields for member functions reading `reflection` (the uniform block's stages
    from the emitter's own `vertexReadsUniforms`/`fragmentReadsUniforms`
    predicates); `ShaderProgram::reflection()`, `ComputeKernel::reflection()`.
    Emission unchanged (every golden passes; `Reflection/reflectingChangesNoSource`
    also renames slots and compares the three dialects). Found while testing:
    `Vector<std::string>::add({})` is the empty initializer-list overload and
    adds nothing - the first naming loop spun forever inside the precompile
    tools at build time. `CompilationInfo/` (12 cases in `GPUCodegenTests`:
    real Metal logs captured on macOS 27, real glslang logs captured from
    eacp-spirv and compared against live compiles where it is built, FXC and
    DXC samples written from the documented formats, unparsed logs, severity
    precedence, `describe`), `CompilationInfoDevice/` (7 in `GPUTests`: a broken
    kernel's line, column and stage on the current backend, a warning kept on
    a success (not Metal), Metal's warning beside an error, a broken fragment
    stage named on Vulkan, every invalid library holding an error (empty
    source, precompiled required and missing), a clean EDSL compile saying
    nothing, a value set breaking only its own D3D compile), `Reflection/` (10
    in `GPUCodegenTests`: a kernel with a member of every binding kind, its
    block and grid extent, a 2D extent, a render program's per-stage
    visibility checked against the emitter's predicates, vertex inputs and
    varyings with interpolation and a carried input, functions and the stages
    calling them, a kernel's struct/matrix/f16/void/uncalled functions and
    overrides of every type, nested/f16/matrix/Bool structs against
    `StructLayout`, a program with no uniforms, reflection and names changing no
    source; plus `Reflection/aShaderProgramNamesItsMembers` in `GPUTests`).
    Unverified: FXC/DXC message parsing against real output, the D3D12 C++
    (`ShaderLibrary-Windows.cpp`, `Dxc-Windows.cpp`, `DxcCompiler-Windows.h`) -
    uncompiled, and the HLSL columns `CompilationInfoDevice/` expects there.
    Not done: positions mapped back to EDSL call sites (a message's line is in
    the emitted source; `source()` is there to read it), Metal pipeline-time
    errors (`newFunctionWithName:constantValues:`, pipeline state creation) as
    messages, precompile-tool (`eacp-shader-compiler`) output as messages,
    reflection of hand-written sources.

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
- Stage 9 part B, first half (f16), as run: macOS (Debug, `build-edsl`,
  `EACP_BUILD_SPIRV=ON`) - the whole project builds; `GPUCodegenTests`
  243/243 (glslang compiles every f16 GLSL source, SPIR-V 1.6 and 1.3),
  `CpuComputeTests` 210/210, `GPUTests` 768/768 on Metal; the test targets
  also build as a unity build (`build-edsl-unity`, batch size 0) and its
  codegen and CPU suites pass. Linux, in the `eacp-ci-linux` image (gcc,
  RelWithDebInfo): `GPUCodegenTests`, `CpuComputeTests` and `GPUTests` (with
  `eacp-gpu` and so the Vulkan changes) built - not the whole project, and not
  through `ci-build`/ctest; the binaries run directly inside `with-weston`:
  `CpuComputeTests` 210/210, `GPUTests` 774/774 on lavapipe (which has both
  f16 features, so the GPU halves ran; `Float16Device/theDeviceAnswersTheQuery`
  now asserts that under `EACP_VK_SOFTWARE=1`), `Float16Device/` again under
  `EACP_VK_VALIDATION=1` with no validation message; `GPUCodegenTests`
  240/243, the three failures `SimdMatrix/{eachBackendSpellsItsOwnWay,
  bfloat16LoadsWithoutStaging,halfLoadsWithoutStaging}/runs` - which fail
  identically at HEAD without these changes (a clean `git archive` of
  4ca1406c built in the same image), so they predate this stage. Not run: the
  Xvfb step, the full Linux `ci-build`.
- The local `eacp-ci-linux` image runs aarch64 on this Mac, where CI's Linux
  lanes are x86_64. Those three `SimdMatrix/*/runs` failures were that: GCC on
  aarch64 contracts the test's own C++ reference into an FMA by default
  (`-ffp-contract=fast`), while `eacp-cpu-compute` is built with
  `-ffp-contract=off`. `CpuComputeTests` already carried the flag;
  `GPUCodegenTests` now does too (`Tests/GPU/CMakeLists.txt`). `GPUTests`' CPU
  halves showed no such failure in the run below, so it was left without.
- Stage 9 part B, second half (structs), as run: macOS (Debug, `build-edsl`,
  `EACP_BUILD_SPIRV=ON`) - the whole project builds; `GPUCodegenTests`
  251/251 (glslang compiles every struct GLSL source, SPIR-V 1.6 and 1.3),
  `CpuComputeTests` 218/218, `GPUTests` 774/774 on Metal (the
  `HiddenWindow` flake did not show), and `GPUWidgetsTests` 77/77,
  `SpirvTests` 10/10, `MLGraphTests` 88/88, `UITests` 371/371; the three test
  targets build as a unity build (`build-edsl-unity`, batch size 0) and its
  codegen (251) and CPU (218) suites pass. Linux, in the `eacp-ci-linux`
  image (aarch64, gcc, RelWithDebInfo, `build-docker-structs`):
  `GPUCodegenTests`, `CpuComputeTests` and `GPUTests` built - not the whole
  project, not through `ci-build`/ctest, no GCC warning from this stage's
  sources - and run directly inside `with-weston`: `CpuComputeTests` 218/218,
  `GPUCodegenTests` 251/251 (the `SimdMatrix` three now pass), `GPUTests`
  780/780 on lavapipe (so `StructDevice/`'s GPU halves, the f16 ones
  included, ran over GLSL std430 and the Vulkan C++), and `StructDevice/`
  again under `EACP_VK_VALIDATION=1` with no validation message. Not run: the
  Xvfb step, the full Linux `ci-build`, any HLSL.
- Stage 10 (interpolation), as run: macOS (Debug, `build-edsl`,
  `EACP_BUILD_SPIRV=ON`) - the whole project builds; `GPUCodegenTests`
  257/257 (glslang compiles every qualified GLSL source, SPIR-V 1.6 and 1.3),
  `CpuComputeTests` 218/218, `GPUTests` 780/780 on Metal (the `HiddenWindow`
  flake did not show); the three test targets build as a unity build
  (`build-edsl-unity`, batch size 0) and its codegen (257), CPU (218) and
  `InterpolationDevice/` (6) suites pass. Linux, in the `eacp-ci-linux` image
  (aarch64, gcc, RelWithDebInfo, `build-docker-structs` reused): the three test
  targets built (no new GCC warning; the six `defined but not used` ones are
  the assert-only helpers already there) - not the whole project, not through
  `ci-build`/ctest - and run directly inside `with-weston` with
  `EACP_REQUIRE_GPU=1 EACP_VK_SOFTWARE=1`: `CpuComputeTests` 218/218,
  `GPUCodegenTests` 257/257, `GPUTests` 786/786 on lavapipe (which has
  `sampleRateShading`, so the per-sample case ran), and `InterpolationDevice/`
  again under `EACP_VK_VALIDATION=1` with no validation message (the layer was
  checked live by the known `SharedMemory/aKernelReportsWhatItDeclares` VUID).
  Not run: the Xvfb step, the full Linux `ci-build`, any HLSL.
- Stage 11 (overrides), as run: macOS (Debug, `build-edsl`,
  `EACP_BUILD_SPIRV=ON`) - the whole project builds with no warning;
  `GPUCodegenTests` 268/268 (glslang compiles every override GLSL source,
  f16 and `INT_MIN` spec constants included, SPIR-V 1.6 and 1.3),
  `CpuComputeTests` 223/223, `GPUTests` 787/787 on Metal (function constants,
  the `is_function_constant_defined` fallback for defaults left out, half
  function constants, and a metallib compiled by `xcrun metal` loaded under the
  require flag and specialised twice), `UITests` 371/371, `GPUWidgetsTests`
  77/77; the three test targets build as a unity build (`build-edsl-unity`,
  batch size 0) and its codegen (268), CPU (223) and `OverrideDevice/` (7)
  suites pass. `Apps/GPU/RotatingTriangle` and `Apps/GPU/Compute` rebuilt -
  their `-shaders` tools ran at build time (manifests rewritten, blobs
  unchanged since their sources are) - and started under
  `EACP_REQUIRE_PRECOMPILED_SHADERS=1` for five seconds each, reporting their
  shader and 3 of 3 kernels precompiled and no error. Linux, in the
  `eacp-ci-linux` image (aarch64, gcc, RelWithDebInfo, `build-docker-structs`
  reused): the three test targets built (no new GCC warning; the six assert-only
  `defined but not used` ones stand) - not the whole project, not through
  `ci-build`/ctest - and run directly inside `with-weston` with
  `EACP_REQUIRE_GPU=1 EACP_VK_SOFTWARE=1`: `CpuComputeTests` 223/223,
  `GPUCodegenTests` 268/268, `GPUTests` 793/793 on lavapipe (so
  `VkSpecializationInfo` on compute and both render stages, a 2-byte f16 spec
  constant, and precompiled SPIR-V specialised at pipeline creation all ran),
  and each `OverrideDevice/` case again under `EACP_VK_VALIDATION=1` with no
  validation message (the layer checked live by the known `SharedMemory/
  aKernelReportsWhatItDeclares` VUID). Not run: the Xvfb step, the full Linux
  `ci-build`, any HLSL, any D3D12.
- Stage 12 (functions), as run: macOS (Debug, `build-edsl`,
  `EACP_BUILD_SPIRV=ON`) - the whole project builds with no warning;
  `GPUCodegenTests` 282/282 (glslang compiles every function GLSL source,
  SPIR-V 1.6 and 1.3), `CpuComputeTests` 231/231, `GPUTests` 794/794 on Metal
  (the `HiddenWindow` flake did not show), `UITests` 371/371,
  `GPUWidgetsTests` 77/77; every function MSL the codegen suite emits (ten
  graphs, MSL's resource parameters, a write-access texture parameter and a
  `discard_fragment()` in a helper among them) also compiled by hand with
  `xcrun metal`; the three test targets build as a unity build
  (`build-edsl-unity`, batch size 0) and its codegen (282), CPU (231) and
  `FunctionDevice/` (7) suites pass; `eacp-gpu-codegen` and
  `eacp-cpu-compute` also compiled as one translation unit each (a
  hand-made unity file, `-fsyntax-only`), since CI's `EACP_CI_BUILD` unity-builds
  the libraries too. Linux, in the `eacp-ci-linux` image (aarch64, gcc,
  RelWithDebInfo, `build-docker-structs` reused): the three test targets built
  (no new GCC warning; the six assert-only `defined but not used` ones stand) -
  not the whole project, not through `ci-build`/ctest - and run directly inside
  `with-weston` with `EACP_REQUIRE_GPU=1 EACP_VK_SOFTWARE=1`:
  `CpuComputeTests` 231/231, `GPUCodegenTests` 282/282, `GPUTests` 800/800 on
  lavapipe, and each `FunctionDevice/` case again under `EACP_VK_VALIDATION=1`
  with no validation message (the layer checked live by the known
  `SharedMemory/aKernelReportsWhatItDeclares` VUID). Not run: the Xvfb step,
  the full Linux `ci-build`, any HLSL, any D3D12.
- Stage 13 (compile messages, reflection), as run: macOS (Debug,
  `build-edsl`, `EACP_BUILD_SPIRV=ON`) - the whole project builds with no
  warning; `GPUCodegenTests` 304/304, `CpuComputeTests` 231/231, `SpirvTests`
  10/10, `GPUTests` 802/802 on Metal (the `HiddenWindow` flake did not show),
  `UITests` 371/371, `GPUWidgetsTests` 77/77; `Apps/GPU/RotatingTriangle` and
  `Apps/GPU/Compute` rebuilt (their `-shaders` tools ran at build time) and
  started under `EACP_REQUIRE_PRECOMPILED_SHADERS=1` - "shader precompiled"
  and "3 of 3 kernels precompiled", no error (Compute prints its plot and exits
  on its own inside the five seconds, as before); the three test targets build
  as a unity build (`build-edsl-unity`, batch size 0) and its codegen (304) and
  CPU (231) suites pass; `eacp-gpu-codegen`, `eacp-cpu-compute` and the C++
  half of `eacp-gpu` each compiled as one translation unit (a hand-made unity
  file, `-fsyntax-only`, clean). Linux, in the `eacp-ci-linux` image (aarch64,
  gcc, RelWithDebInfo, `build-docker-structs` reused): `GPUCodegenTests`,
  `CpuComputeTests`, `GPUTests` and `SpirvTests` built (no new GCC warning; the
  six assert-only `defined but not used` ones stand) - not the whole project,
  not through `ci-build`/ctest - and run directly inside `with-weston` with
  `EACP_REQUIRE_GPU=1 EACP_VK_SOFTWARE=1`: `SpirvTests` 10/10,
  `CpuComputeTests` 231/231, `GPUCodegenTests` 304/304 (the live glslang case
  included), `GPUTests` 808/808 on lavapipe (so glslang's per-stage messages,
  the warning on a success and the fragment-stage naming ran through
  `ShaderLibrary-Linux.cpp`), and `CompilationInfoDevice/` again under
  `EACP_VK_VALIDATION=1` with no validation message (the layer checked live by
  the known `SharedMemory/` VUID). Not run: the Xvfb step, the full Linux
  `ci-build`, any HLSL, any D3D12, FXC/DXC output.
- Windows: never built. No FXC or DXC on the Mac, so no emitted HLSL has
  been compiled and the D3D12 C++ is uncompiled; every "by documentation"
  note above stands until the Windows lanes (FXC, and the D3D12 suites on
  WARP) check it. That includes the f16 HLSL (`eacpRoundHalf`, the
  `InterlockedAnd`/`InterlockedOr` half stores) and `Device-Windows.cpp`'s
  `supportsShaderFloat16`, and the struct HLSL (`[RW]ByteAddressBuffer` loads
  and stores at the computed offsets, the two-argument `InterlockedAnd`/
  `InterlockedOr` methods for f16 fields, `eacpMake<Name>` helpers), and the
  interpolation modifiers (`noperspective`, `centroid`, `sample`,
  `nointerpolation` on a float) with `Device::supportsSampleRateShading`, and
  the override HLSL (`#ifndef`/`#define` defaults, `static const` of a macro,
  FXC `D3D_SHADER_MACRO`s and DXC `-D` per value set) with the D3D12 variant
  cache in `ShaderLibrary-Windows.cpp` and the pipelines'
  `nativeLibrary(constants)`, and the function HLSL (`fn_<name>` definitions
  over the global resources, value returns from loops and switch clauses,
  `discard` in a helper, matrix parameters in HLSL's row-first convention) -
  documentation only, no line of it compiled. Stage 12 changed no D3D12 C++.
  Stage 13 did, uncompiled: `ShaderLibrary-Windows.cpp` (FXC's blob parsed on
  success and failure, `StageTarget::stage`, per-variant `compilation`,
  `variantFor`, `compilationInfo(constants)`), `Dxc-Windows.cpp` and
  `DxcCompiler-Windows.h` (`compile` taking the stage and a
  `CompilationInfo&`); `parseFxcLog`/`parseDxcLog` are tested only against
  sample text written from the documented formats, and the HLSL lines and
  columns `CompilationInfoDevice/` expects are FXC's documented ones.
