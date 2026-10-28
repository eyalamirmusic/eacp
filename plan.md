# D3D12 shaders through DXC at shader model 6 — plan

Written 2026-10-27 against `38f9146d` (`develop`), from a read of the D3D12
shader path (`Shader/ShaderLibrary-Windows.cpp`, `Pipeline/*-Windows.cpp`,
`Windows/D3D12Context-Windows.cpp`, `Windows/D3D12Types.h`), the HLSL half of
the emitter (`Codegen/ShaderEmitter.cpp`, `ShaderBuilder*.cpp`), the
reduction fold in `CpuCompute/Statements.cpp`, the GPU test CMake and
`CpuCrossCheck.h`, the CMake helpers (`TargetSetup.cmake`, `EacpPlugin.cmake`,
`FindWebView2.cmake`), `Core/Plugins/ModuleInfo*`, the CI workflow, and the
DXC and WARP release material listed in §1. Paths are under `Lib/eacp/GPU/`
unless they say otherwise. Line counts are estimates, not commitments.

## Progress

| Phase | State | Notes |
| --- | --- | --- |
| 0 — facts and measurements | not started | §1 has the web-verified facts; the device and timing numbers need a Windows machine |
| 1 — DXC beside FXC, behind a switch | not started | |
| 2 — parity, DXC the default | not started | |
| 3 — wave fast path for the folds | not started | |
| 4 — fp16 arithmetic in the EDSL | not started, needs a decision (§3, D3) | |
| 5 — no runtime compiler (optional) | not started | |

Where the code differs from the sketches below, the code wins; mark each such
point *as built* in place, as the last plan did.

## 0. Why

The D3D12 backend compiles every shader with FXC (`D3DCompile`) to DXBC at
`vs_5_0`/`ps_5_0`/`cs_5_0`. That model is frozen: no wave intrinsics, no
16-bit arithmetic (`half` is `float`), a 4096-register budget for indexable
temporaries that has already shaped the SIMD-matrix fallback (README
"SIMD-group matrix", `error X4505`), and no access to anything added since
2016. Its consequences are written into the README in four places: the
reductions walk a `groupshared` tree with a barrier per halving step on D3D12
(README l.1864-1872, 1890-1897), the SIMD-group matrix stages through
threadgroup memory (l.2004-2027), there is no `Half` value type "and there is
not going to be one" (l.2286-2290), and D3D12's vertex and instance ids cannot
see the draw's start locations (l.327).

DXC compiles HLSL to DXIL at shader model 6.x. The user has fixed the terms:
no Agility SDK — its `D3D12SDKVersion` export has to live in the `.exe`, and
eacp ships inside other people's executables as a plugin — so the usable
shader model is whatever the inbox runtime and the driver report, queried at
run time and never assumed; DXC ships as a DLL beside the module, not linked
in; and wave operations and native 16-bit types are capability-gated fast
paths over the code that runs today.

## 1. Verified facts

Checked on 2026-10-27. "Confirmed" means a primary source says it; "secondary"
means a forum or third-party report; "unverified" means Phase 0 has to measure
it.

| # | Fact | Status | Source |
| --- | --- | --- | --- |
| F1 | DXC ≥ 1.8.2502 no longer needs the validator binaries (`dxil.dll`) for "signed" (hashed) shaders: the DXIL validator hash is open-sourced into `dxcompiler.dll`. 1.8.2502 still *loads* `dxil.dll` if it finds one; from 1.8.2505 "the compiler will now always use the internal validator instead of searching for an external DXIL.dll". So with ≥ 1.8.2505 only `dxcompiler.dll` ships, and no name-based `dxil.dll` search can pick up a host's copy | confirmed | [v1.8.2502 notes](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502), [release list / v1.8.2505 notes](https://github.com/microsoft/DirectXShaderCompiler/releases), `ReleaseNotes.md` in the NuGet package (l.274, l.314) |
| F2 | Current releases: v1.9.2609 (2026-09-29) is the newest stable; v1.10.x are SM 6.10 previews. v1.9.2609 fixed the Windows ARM64 import libraries | confirmed | [v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609) |
| F3 | The GitHub zip `dxc_2026_09_29.zip` is 32.3 MB and holds `bin/{x64,arm64,x86}/{dxcompiler.dll,dxil.dll,dxc.exe,dxv.exe}`, `inc/dxcapi.h` and friends, `lib/*`. `dxcompiler.dll` is 22.3 MB (x64) and 23.7 MB (arm64); `dxil.dll` ~2 MB. The NuGet `Microsoft.Direct3D.DXC` 1.9.2609.5 is 53 MB (the same binaries plus PDBs under `build/native/bin/<arch>/`). A Linux x86_64 tarball (13 MB, `libdxcompiler.so`) exists; no macOS or Linux arm64 build | confirmed (downloaded and listed) | [release assets](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609), [NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.DXC) |
| F4 | `dxcompiler.dll`'s static imports are system DLLs only (KERNEL32, ADVAPI32, ole32, OLEAUT32). The string `dxil.dll` still appears in the image, so Phase 0 confirms with a loader trace that 1.9.2609 never tries to open it | confirmed (string scan) / unverified (runtime) | local scan of the x64 DLL |
| F5 | `IDxcBlob` is "an alias of ID3D10Blob and ID3DBlob" — same IID `8BA5FB08-…` — so `D3D12ShaderProgram`'s `winrt::com_ptr<ID3DBlob>` members can hold DXC output unchanged | confirmed | `inc/dxcapi.h` l.147-150 of v1.9.2609 |
| F6 | The package is redistributable: `LICENSE-MS.txt` grants distribution of the listed distributables with an application; the compiler itself is LLVM/MIT | confirmed | `LICENSE-MS.txt` §"Distributable code" in the zip |
| F7 | Inbox runtime shader models: Windows 10 2004 (19041) added SM 6.5, so 22H2 (19045) tops out at 6.5; Windows 11 added SM 6.6. Later Windows 11 builds report higher (a user report has 25H2 at 6.8) | 10 2004/11: confirmed; 22H2 = 6.5: inferred; 24H2/25H2: secondary | [What's new in D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/new-releases), [O3DE issue](https://github.com/o3de/sig-graphics-audio/issues/154), [Guru3D report](https://forums.guru3d.com/goto/post?id=6365537) |
| F8 | `D3D12_FEATURE_SHADER_MODEL`: the caller seeds `HighestShaderModel` with the highest it understands and gets back the min of that and the device's; `CheckFeatureSupport` returns `E_INVALIDARG` if the seed is unknown to the runtime, so the query is a loop downward from the header's newest value | confirmed | [D3D12_FEATURE_DATA_SHADER_MODEL](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_shader_model) |
| F9 | Inbox WARP on Windows 11 (as of 2022) "only supports up to shader model 6.2", and had "several wave ops that produced incorrect results" fixed only in the redistributable WARP. The redistributable is `Microsoft.Direct3D.WARP` (1.0.21, 2026-09-22; arm64 since 1.0.12), dropped beside the test executable as `d3d10warp.dll` | confirmed (2022 statement); what the CI images' WARP reports today is unverified | [WARP SM 6.7 preview](https://devblogs.microsoft.com/directx/announcing-warp-preview-with-shader-model-6-7-support/), [NuGet WARP](https://www.nuget.org/packages/Microsoft.Direct3D.WARP) |
| F10 | WARP's wave size: "Minimum wave size is still 4"; from WARP 1.0.16 the maximum is 8 with AVX2 and 16 with AVX-512. **No WARP runs a 32-lane wave**, so a fast path gated on waves ≥ 32 would never execute in CI | confirmed (redistributable); inbox value unverified, expected 4 | NuGet WARP 1.0.16 release notes |
| F11 | `WaveGetLaneCount()` is between 4 and 128 and driver-dependent; `WaveLaneCountMin` can understate what a shader actually runs at (Intel Gen12LP reports 16 and runs fragments at 8). `[WaveSize]` is SM 6.6, out of reach on Windows 10 | confirmed | [WaveGetLaneCount](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavegetlanecount), [Dawn change](https://dawn.googlesource.com/dawn/+/refs/heads/chromium/6877), [SM 6.6 spec](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_6.html) |
| F12 | `WaveReadLaneAt(expr, lane)` with a non-uniform lane index is a shuffle; undefined only if the named lane is inactive. SM 6.0, all stages | confirmed | [WaveReadLaneAt](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavereadlaneat) |
| F13 | Native 16-bit needs SM 6.2, `-enable-16bit-types` and `D3D12_FEATURE_DATA_D3D12_OPTIONS4::Native16BitShaderOpsSupported`; Dawn gates `shader-f16` on exactly that | confirmed | [Dawn D3D12Info.h](https://dawn.googlesource.com/dawn.git/+/chromium/6594/src/dawn/native/d3d12/D3D12Info.h) |
| F14 | HLSL 2021 is DXC's default since 1.7.2212. Breaking changes against 2018: `&&`/`||` short-circuit on scalars and are scalar-only, vector `?:` is an error (use `select()`), `and`/`or`/`select` become intrinsic names, `for`-statement variables scope to the loop, user-defined types no longer convert by layout. HLSL 202x (opt-in, `-HV 202x`) changes literal typing and `[unroll(N)]` | confirmed | [Announcing HLSL 2021](https://devblogs.microsoft.com/directx/announcing-hlsl-2021/), [Language versions](https://github.com/microsoft/DirectXShaderCompiler/wiki/Language-Versions), NuGet `ReleaseNotes.md` l.50, 339-348, 391 |
| F15 | Precedent: Dawn loads `dxcompiler.dll` relative to its own module (`GetModuleDirectory`) "to prevent loading issues … when the executable is not in the same path as the dawn module", enables DXC only when the DLL loads, and later removed the Windows SDK fallback for security | confirmed (Dawn history) | [use_dxc toggle](https://dawn.googlesource.com/dawn/+/20ed6f42b34f4989022a2a3c87f057d90a6618dd%5E%21/), [Remove DXC fallbacks to WinSDK](https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7303) |
| F16 | Which D3D12 hardware/driver pairs report < SM 6.0 (Fermi, Haswell/Broadwell, Parallels' virtual GPU, pre-1607 Windows) | unverified — no primary source found | — |
| F17 | FXC (`d3dcompiler_47.dll`) is inbox on Windows 10/11, so the FXC fallback costs no deployment | known; not re-checked | — |

## 2. What is there

**The compile.** `Shader/ShaderLibrary-Windows.cpp` `compileStage` (l.23-50)
is one `D3DCompile` call with `D3DCOMPILE_ENABLE_STRICTNESS`, logging the
error blob on failure; `ShaderLibrary::Native` (l.55-81) calls it at `cs_5_0`
(l.67) or `vs_5_0` + `ps_5_0` (l.72, 76) and keeps the blobs in a
`D3D12ShaderProgram` (`Windows/D3D12Types.h` l.157-162: three
`com_ptr<ID3DBlob>`). There is no reflection anywhere (`D3DReflect` is never
called): vertex inputs match by `TEXCOORD<n>` semantics
(`Pipeline/RenderPipeline-Windows.cpp` l.166-189) and bindings are fixed
registers, so nothing downstream reads the bytecode beyond pointer and size.

**Where the bytes go.** `Pipeline/ComputePipeline-Windows.cpp` l.26-37 and
`Pipeline/RenderPipeline-Windows.cpp` l.523-549 copy pointer and size into
the PSO desc against the shared root signatures. Neither cares what the blob
is; DXBC and DXIL go through the same field.

**Root signatures.** `D3D12Shared::createRootSignatures`
(`Windows/D3D12Context-Windows.cpp` l.596-797), serialised at
`D3D_ROOT_SIGNATURE_VERSION_1` by `makeRootSignature` (l.504-523; the
serialiser is in `d3d12.dll`, not the compiler). Register space 0 only,
fixed registers (`D3D12Types.h`), eight static samplers at `s0..s7`, per-bind
sampler tables at `s8+` unless `DriverQuirks::samplerTablesIgnoreOffset`. The
signature is external to the shader, which DXIL accepts as DXBC does, so it
does not change. `SAMPLERS.md` l.175-178 records why there are no register
spaces: "they need 5.1 and bgfx's DXBC is 5.0" — the signature is
deliberately SM 5.0-compatible and stays so, since DXBC and DXIL shaders bind
against one signature.

**Internal shaders.** Two more `D3DCompile` copies in
`D3D12Context-Windows.cpp`: the depth-resolve stand-in (`depthResolveSource`
l.851-872, compiled at `vs_5_0`/`ps_5_0` l.874-899, 924-925), used only when
`DriverQuirks::noDepthResolve`; and the mip generator (`mipSource` l.959-1037,
three kinds behind `MIP_KIND`, `cs_5_0` with `D3D_SHADER_MACRO` defines,
l.1039-1073). The depth resolve names a loop variable `sample` (l.867), an
HLSL interpolation-modifier keyword that FXC accepts as an identifier; rename
it rather than find out what DXC thinks.

**Device creation.** `createHardwareOrWarpDevice` (l.381-415) creates at
`D3D_FEATURE_LEVEL_11_0`, falling back to WARP (`EACP_D3D12_WARP=1` forces
it, l.67). `probeDriverQuirks` (l.335-379) runs before it on a throwaway
device. Nothing queries `D3D12_FEATURE_SHADER_MODEL`, `OPTIONS1` or `OPTIONS4`
today. `describeAdapter` (l.461-483) is the natural place to log what the new
queries find.

**The emitter.** Device-free by design: `ShaderBuilder::build()`
(`Codegen/ShaderBuilder.cpp` l.121-123) calls
`detail::nativeShaderSource(graph)`, which on Windows is
`ShaderSource::hlsl(emitHlsl(graph))` (`Codegen/ShaderBuilder-Windows.cpp`
l.9-12), and the result is `ComputeKernel::source()` — produced before any
`Device` is in sight, and the same text `GPUCodegenTests` asserts on. **This
is the constraint that shapes Phases 3 and 4**: the HLSL cannot be emitted
per device capability without moving emission into `prepare(device)`. The
plan keeps emission device-free and puts the fast paths behind preprocessor
symbols the compile step defines (§4.3).

FXC-shaped decisions in the emitter, all commented as such:
`groupReduction` (`ShaderEmitter.cpp` l.2465-2550, "HLSL under FXC has no
wave intrinsic at cs_5_0"); the `SV_GroupIndex` semantic as the flattened
lane (l.3747-3753); the SIMD-matrix lane layout of two floats per lane
because of FXC's 4096-register budget (l.1138-1150); the erf helper written
as overloads because HLSL "has no template before shader model 6" (l.236-246).
None of it is wrong under DXC; each is a place a fast path or a later cleanup
attaches. `Select` emits `(c ? a : b)` with a scalar `Bool` condition always
(`ShaderValue.h` l.1938-1988 takes `const Bool&`), and `&&`/`||` take scalar
`Bool`s (l.1878-1885), so the HLSL 2021 scalar-only rules (F14) are already
met; short-circuiting `?:` and `&&` changes nothing observable for
side-effect-free operands. The cbuffer is a padded struct inside
`cbuffer UniformsCB : register(b0)` (l.3222-3276), the legacy layout DXC
keeps by default.

**The fold.** `simdGroupWidth` is 32 (`Codegen/ShaderGraph.h` l.216).
`reductionStride` (l.958-966) is the largest power of two below the width;
the tree steps `stride, stride/2, …, 1` with `within < step && within + step
< width`. `CpuCompute/Statements.cpp` `foldHalving` (l.254-267) folds in the
same order, which is why a float sum on the CPU rounds as D3D12 and Vulkan do
(README l.2656). Metal already folds the hardware SIMD group in its own order
and the tests carry tolerances for it (`Tests/GPU/GroupReductionTests.cpp`
`.agreeing(1e-3f … 1e-1f)`; `CpuCrossCheck.h` l.59-71, 243).
`ComputePipeline::threadExecutionWidth()` answers 0 on D3D12
(`ComputePipeline-Windows.cpp` l.55-60), which is what keeps
`ComputeProgram::prepare`'s width warning quiet there.

**Hand-written HLSL.** `nativeShaderSource(msl, hlsl, glsl)` /
`nativeComputeShaderSource` (`Tests/GPU/Common.h` l.23-45) in 18 GPU test
sources; `Apps/GPU/Triangle/Triangle.hlsl`. (`VideoRecorderGpu` and
`TriangleGen` use the EDSL and carry no HLSL.)

**Tests.** `GPUCodegenTests` (`Tests/GPU/CMakeLists.txt` l.3-21) checks
emitted HLSL as strings and, where `eacp-spirv` exists, compiles the GLSL
through `eacp_enable_glsl_compile_checks`; nothing compiles the HLSL without
a device. `GPUTests` (l.148-154) is where every HLSL compile happens today,
on WARP in CI.

**CMake.** `eacp-gpu` links `d3dcompiler` PRIVATE (`Lib/eacp/GPU/CMakeLists.txt`
l.125-131). eacp-gpu is static, so a runtime DLL loaded by `LoadLibraryExW`
is no link dependency and `$<TARGET_RUNTIME_DLLS>` will not see it: something
has to copy it beside every executable and module that links `eacp-gpu`.
`eacp_add_app` (`CMake/TargetSetup.cmake` l.46), `eacp_add_plugin`
(`CMake/EacpPlugin.cmake` l.11) and `nano_add_executable` (NanoTest) are the
three ways such targets are made; `eacp_collect_link_targets`
(`EacpPlugin.cmake` l.34) already walks a link closure. `CMake/FindWebView2.cmake`
is the in-tree precedent for a Microsoft binary package fetched at configure
time with architecture picked from `CMAKE_CXX_COMPILER_ARCHITECTURE_ID`.

**Finding our own directory.** `Plugins::getCurrentModulePath()`
(`Core/Plugins/ModuleInfo.h` l.14, `ModuleInfo-Windows.cpp` l.28) answers
for the image holding this eacp copy, resolved from a code address — the
plugin DLL inside a DAW, the executable otherwise. Exactly what the loader
needs.

**Docs.** No line of `CLAUDE.md` or `Docs/Build.md` names FXC or a shader
model; the FXC/SM5 statements are all in `Lib/eacp/GPU/README.md` (l.327,
1660, 1864-1872, 1890-1897, 2004-2027, 2286-2290) and `SAMPLERS.md` l.175-178.
`CLAUDE.md` and `Build.md` need the new dependency and switches, not
corrections.

## 3. Decisions

Made by the user: SM 6 through DXC; no Agility SDK; `dxcompiler.dll` as a
runtime DLL; waves and 16-bit as gated fast paths.

**D1 — keep FXC as the fallback, or require SM 6.** *Recommendation: keep it.*

- *Keep:* when `dxcompiler.dll` is missing beside the module, fails to load,
  or the device reports < SM 6.0 or no `WaveOps`, the device compiles with
  FXC at SM 5.0 exactly as today. The cost is small because the plan keeps
  one emitted text: the base HLSL stays the common subset both compilers take
  (it already is), and SM 6-only code exists only inside `#if` blocks the
  compile step enables (§4.3). What it costs is the `d3dcompiler` link, the
  compiler-selection code (~60 lines), one CI configuration that keeps the
  FXC path tested (§7), and the discipline that the base dialect never uses
  templates or other SM 6-only syntax. `Half` programs (Phase 4) refuse to
  build on an FXC device rather than degrade.
- *Drop:* simpler code and SM 6 syntax anywhere, but any device that reports
  < 6.0 (F16: unknown population — old Intel and NVIDIA drivers, VMs, very
  old Windows 10) gets `Device::isValid() == false`, and so does every plugin
  whose installer forgot a 22 MB DLL. For a library shipped inside DAWs by
  third parties the second case is the likelier one.

Revisit after Phase 0's capability survey and a release or two of DXC-default
running; dropping FXC later is a deletion, adding it back is not.

**D2 — the compiler is chosen once per device, not per shader.** The root
signatures are built at device creation and one of them depends on a driver
quirk that was found under DXBC (`samplerTablesIgnoreOffset`, Adreno). If a
quirk turns out to be DXBC-only, its detection must know which compiler the
device uses, which means deciding the compiler before `createRootSignatures`.
Mixing (DXC for some shaders, FXC for others) would also make "which compiler
ran" a per-pipeline question in every bug report. One choice per device, made
in `D3D12Shared::createDevice`, logged with the adapter.

**D3 — what `Half` means (Phase 4, open).** Either (a) *exact*: a `Half`
program builds only where the device does binary16 arithmetic, and
`ComputeProgram::prepare` refuses elsewhere with a reason, the app keeping a
`Float` twin behind `Device::supportsHalfArithmetic()` — the pattern
`supportsBFloat16SimdMatrix()` already set (README l.2066-2080); or (b)
*relaxed*: `min16float` semantics, "at least fp16 precision", fp32 where
there is no fp16. *Recommendation: (a)*. It keeps the README's argument
against `Half` (one declaration meaning two things) answered rather than
ignored, gives `CpuCompute` one semantics to implement, and leaves (b)
available later as an explicit opt-in. Either way it reverses a written
"there is not going to be one", which is the user's call.

**D4 — pin the language version.** Pass `-HV 2021` explicitly rather than
inherit DXC's default, so a DXC upgrade that moves the default to 202x
(literal typing, `[unroll(N)]` semantics, F14) is a deliberate change.

## 4. Design

### 4.1 Loading DXC

A new `Windows/DxcCompiler-Windows.{h,cpp}` (~200 lines) owns the library:

- Path: `Plugins::getCurrentModulePath().parent() / "dxcompiler.dll"`, loaded
  with `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
  LOAD_LIBRARY_SEARCH_SYSTEM32)`. A full path, so the DAW's own directory,
  `PATH` and the current directory are never searched (the DLL-planting
  surface Dawn closed, F15), and two eacp copies in one process each get the
  file beside their own image. `EACP_DXC_PATH` overrides the directory, for
  local work against another DXC.
- Once per eacp copy, lazily, on the first device creation that wants it;
  never `FreeLibrary`'d. A plugin unloaded by its host leaves the mapping
  behind until process exit; freeing it from a static destructor would run
  under the loader lock (`DLL_PROCESS_DETACH`), which is the worse bug. A
  reload from the same path gets the same handle back.
- `GetProcAddress("DxcCreateInstance")`, then `IDxcUtils` and
  `IDxcCompiler3`. One compiler instance behind a mutex: compiles are not hot
  enough to justify per-thread instances, and a lock is cheaper than finding
  out which DXC interfaces are thread-safe (Phase 0 measures contention in
  `GPUTests -j`).
- Version check through `IDxcVersionInfo`: refuse (fall back, log) below
  1.8.2505, the first release that never looks for `dxil.dll` (F1).
- The headers come from the fetched package (`inc/dxcapi.h`), not the Windows
  SDK's older `um/dxcapi.h`; target include directories precede the `INCLUDE`
  environment, so the fetched one wins on both MSVC and clang-cl.

### 4.2 Choosing the compiler

`D3D12Shared::createDevice` gains a `ShaderCompilerCaps` (in `D3D12Context.h`,
beside `DriverQuirks`):

```cpp
struct ShaderCompilerCaps
{
    bool dxc = false;               // DXC loaded and the device takes DXIL
    int shaderModel = 50;           // 50, 60, 61, ... as compiled for
    bool waveOps = false;           // OPTIONS1.WaveOps
    int waveLanesMin = 0, waveLanesMax = 0;
    bool native16Bit = false;       // SM >= 6.2 and OPTIONS4 flag
};
```

The shader model is the F8 loop: seed with the newest `D3D_SHADER_MODEL_*`
the build's headers know, step down on `E_INVALIDARG`. `dxc` is true when the
library loaded, passed the version check, and the device reports ≥ 6.0 with
`WaveOps`. Compile at `min(reported, 6.5)` at first — the ceiling Windows 10
22H2 can run (F7) — so a shader compiled on a developer's Windows 11 machine
means the same thing on a user's Windows 10 one; raising the cap is a later,
deliberate step. `EACP_D3D12_COMPILER=fxc|dxc` overrides, in the style of
`EACP_D3D12_WARP` and `EACP_D3D12_QUIRKS` (`dxc` on a device that cannot take
it logs and falls back). `describeAdapter`'s line grows the caps, so every CI
log and bug report says which compiler ran.

### 4.3 One compile front, and the predefined symbols

`Windows/HlslCompiler-Windows.{h,cpp}` (~150 lines) replaces the three
`D3DCompile` copies:

```cpp
winrt::com_ptr<ID3DBlob> compileHlsl(const std::string& source,
                                     const std::string& entry,
                                     ShaderStage stage,
                                     const Vector<HlslDefine>& defines = {});
```

It reads the device's `ShaderCompilerCaps` and either calls `D3DCompile` at
`*_5_0` with `D3DCOMPILE_ENABLE_STRICTNESS` (today's call) or `IDxcCompiler3::
Compile` with `-T cs_6_x` / `vs_6_x` / `ps_6_x`, `-E <entry>`, `-HV 2021`,
`-Ges`, `-O3` (`-Zi -Qembed_debug` in Debug, for PIX), plus
`-enable-16bit-types` when `native16Bit` and the source asks for it. The
output is queried as `IDxcBlob` and held as `ID3DBlob` (F5); errors are
logged exactly as now, warnings dropped as now.

Every compile, FXC or DXC, defines:

| Symbol | Value |
| --- | --- |
| `EACP_HLSL_SM` | `50`, `60`, `62`, … |
| `EACP_WAVE_OPS` | `1` when the device runs wave intrinsics through DXC, else undefined |
| `EACP_NATIVE_16BIT` | `1` when compiled with `-enable-16bit-types` |

The emitter writes fast paths as `#if defined(EACP_WAVE_OPS) … #else … #endif`
around today's code, so emission stays device-free, `CpuCompute` and the
Metal/GLSL emitters are untouched, `GPUCodegenTests` sees both branches in one
string, and hand-written HLSL can use the same symbols. The symbols are the
whole interface between emitter and device; nothing else crosses.

### 4.4 Deployment

`CMake/FindDXC.cmake` (~70 lines, `FindWebView2.cmake`'s shape): on `WIN32`
with `EACP_D3D12_DXC` on,

```cmake
CPMAddPackage(NAME DXC
        URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
        URL_HASH SHA256=<pinned>
        DOWNLOAD_ONLY YES)
```

then an `eacp-dxc` INTERFACE target carrying `inc/` and a property
`EACP_DXC_DLL` = `bin/<x64|arm64>/dxcompiler.dll`, the architecture taken from
`CMAKE_CXX_COMPILER_ARCHITECTURE_ID` as `FindWebView2.cmake` does. Pinned by
version and hash, not `main`, unlike most of eacp's CPM packages: a compiler
upgrade changes generated code. Nothing is compiled or linked from it, so
MSVC and clang-cl lanes take it identically and ccache is not involved. Only
`dxcompiler.dll` ships (F1).

`eacp_deploy_gpu_runtime(<target> [DESTINATION <dir>])` (public, in
`CMake/TargetSetup.cmake`) adds a `POST_BUILD` `copy_if_different` of the DLL
to `$<TARGET_FILE_DIR:target>` (or `DESTINATION`, for a VST3 bundle's
`Contents/x86_64-win/`), and an `install(FILES …)` beside the target's own.
It is called by `eacp_add_app` and `eacp_add_plugin` when the target links
`eacp-gpu` — checked after the fact with `cmake_language(DEFER CALL …)` and
`eacp_collect_link_targets`, since the link is added after the call — and
explicitly for the test executables that link `eacp-gpu` (`GPUTests`,
`UITests`, `GPUWidgetsTests`, the Sprites and Text suites, and any other the
DEFER walk finds; a configure-time warning names any executable or module in
the tree that links `eacp-gpu` without having been deployed to). A plugin
project outside the tree calls it on its own wrapper target.

Size: +22.3 MB (x64) / +23.7 MB (arm64) beside each app and each plugin (F3).
Every plugin carries its own copy, which is the point; Phase 5 is the way out.

## 5. Phases

### Phase 0 — facts and measurements

No production code. Needs a Windows x64 machine with a GPU, one Windows on
Arm machine (Adreno), and the CI runners.

1. **Capability survey.** A throwaway console (not committed) or a temporary
   log in `describeAdapter`: highest shader model (F8 loop), `OPTIONS1`
   `WaveOps`/`WaveLaneCountMin`/`Max`, `OPTIONS4` native 16-bit, on: CI's
   `windows-latest` and `windows-11-arm` WARP; Windows 10 22H2 on a GPU;
   Windows 11 24H2/25H2 on NVIDIA, AMD (wave32/64), Intel (variable width,
   F11) and Adreno; a Parallels VM (the driver `DriverQuirks` was written
   for); and the redistributable WARP (F9) dropped beside a test binary.
   Record the table in this plan. Settles F7, F9-F11, F16 for the machines
   eacp actually meets.
2. **Offline compile of every HLSL source eacp produces.** A temporary
   `EACP_D3D12_DUMP_HLSL=<dir>` in `compileStage` writing each source with its
   entry and target; run `GPUTests`, `UITests`, `GPUWidgetsTests` and the GPU
   apps headless on WARP; then run `dxc.exe -HV 2021 -T <stage>_6_0` over the
   dump (Windows, or the Linux `dxc` in the CI Docker image). The result is
   the list of FXC-isms Phase 1 must fix, emitted and hand-written; the
   `sample` loop variable (§2) is the one candidate already known.
3. **Compile time and size.** Over the same dump, per source: FXC and DXC wall
   time (Release, x64), DXBC and DXIL byte size, and the first-call cost of
   loading `dxcompiler.dll` and creating the instance. Then the wall time of
   `GPUTests` on WARP each way, once Phase 1 exists. The SIMD-matrix blocked
   product that took FXC "seconds" (README l.2022-2027) is the interesting
   row.
4. **Loader behaviour.** Under Process Monitor, confirm DXC 1.9.2609 never
   probes for `dxil.dll` (F4), and that two copies of `dxcompiler.dll` at
   different paths load side by side when each is loaded by full path.
5. **Lane mapping.** On each GPU in item 1, a hand-written SM 6.0 kernel that
   writes `SV_GroupIndex`, `WaveGetLaneIndex()` and `WaveGetLaneCount()` for
   groups of 32, 64, 128 and 256: does every vendor put group lanes
   `[k·W, (k+1)·W)` in one wave at lane `i mod W`? Phase 3 rests on it.

*Done:* the tables in this plan; the FXC-ism list; a go/no-go on F16 that
feeds D1.

### Phase 1 — DXC beside FXC, behind a switch

1. `CMake/FindDXC.cmake`, `EACP_D3D12_DXC` (option, default `ON` on `WIN32`,
   meaningless elsewhere), `eacp-dxc`; `eacp-gpu` links it PRIVATE on `WIN32`
   (`Lib/eacp/GPU/CMakeLists.txt` l.108-131, beside `d3dcompiler`).
2. `eacp_deploy_gpu_runtime` and its calls (§4.4); the DEFER walk and warning.
3. `Windows/DxcCompiler-Windows.{h,cpp}` (§4.1) and
   `Windows/HlslCompiler-Windows.{h,cpp}` (§4.3), added to the `WIN32`
   `target_sources` block.
4. `ShaderCompilerCaps` in `D3D12Context.h`, filled in
   `D3D12Shared::createDevice` (`D3D12Context-Windows.cpp` l.587-594) after
   the device exists and before `createRootSignatures`; exposed as
   `D3D12Shared::getCompilerCaps()`; logged by `describeAdapter`.
5. `ShaderLibrary-Windows.cpp`: `compileStage` becomes a call into
   `compileHlsl`; the comment at l.11-16 rewritten. `compileDepthResolveStage`
   and `compileMipKernel` likewise (their `D3D_SHADER_MACRO` arrays become
   `HlslDefine`s); rename the depth resolve's `sample`.
6. Default `EACP_D3D12_COMPILER` = `fxc` in this phase: nothing changes for
   anyone who does not set it.
7. Fix what Phase 0 item 2 found, in the emitter and the hand-written test
   and example HLSL, keeping every fix valid under FXC.
8. Public surface: `Device::shaderModel()` (`50` off D3D12's DXC path and on
   every other backend that has no such number — or `0`; decide in review)
   so a hand-written-HLSL user can choose a source; documented beside
   `maxComputeBufferSlots`.
9. Tests, all Windows-only and new:
   - `Tests/GPU/ShaderCompilerTests-Windows.cpp`: the DLL was loaded from
     beside the test binary (`GetModuleFileNameW` on the module); a missing
     DLL (`EACP_DXC_PATH` at an empty directory) falls back to FXC and still
     draws; the predefined symbols reach both compilers; a DXIL compute
     pipeline builds against the shared root signature and dispatches (so the
     hash is accepted, F1); `EACP_D3D12_COMPILER=fxc` wins over a working DXC.
   - `eacp_enable_hlsl_compile_checks(GPUCodegenTests)`, the twin of the GLSL
     helper (`Tests/GPU/CMakeLists.txt` l.14-21): on Windows, every emitted
     HLSL string is compiled by DXC at 6.0 and by FXC at 5.0 inside the suite,
     with no device — the same move `eacp-spirv` made for GLSL. Optional
     extension: the Linux `libdxcompiler.so` (F3) on the x86_64 Linux lanes,
     which would put HLSL compile checks on three more lanes; not required.

*Done:* every Windows lane green with the default (FXC) unchanged; `GPUTests`,
`UITests` and `GPUWidgetsTests` run under `EACP_D3D12_COMPILER=dxc` on WARP on
one x64 lane with failures listed, not necessarily fixed; the new tests green
on all four Windows lanes; Phase 0 item 3's suite-time numbers recorded.

### Phase 2 — parity, DXC the default

1. Every D3D12 suite green under `EACP_D3D12_COMPILER=dxc` on WARP on all four
   lanes (x64/ARM64 × MSVC/clang-cl). Expect the numeric differences to come
   from fp32 denormals (DXBC flushes in arithmetic; SM 6.0 DXIL is
   `-denorm any`, the driver's choice) and from the "undefined, or all-ones
   on D3D" integer divide by zero (README l.2669), which DXIL leaves
   undefined; fix tests that pinned DXBC behaviour by widening them to the
   documented GPU column, not by special-casing DXC.
2. Hardware pass on the Phase 0 machines, `GPUTests` and `UITests`, each
   compiler. The driver-quirk review:
   - `samplerTablesIgnoreOffset` (Adreno): run with
     `EACP_D3D12_SAMPLER_TABLES=1` under DXC. If DXIL samples correctly, make
     `detectsSamplerTableBug` (l.307-318) compiler-aware — the reason D2 puts
     the choice before the root signatures — and say so in `SAMPLERS.md`.
   - `noDepthResolve`, `noBoxedTextureCopy` (Parallels): command-list
     operations, not shader ones; expected unchanged, but the depth-resolve
     fallback shader now compiles through DXC there, so run
     `EACP_D3D12_QUIRKS=1` on WARP and the real Parallels driver.
   - The eight fixed `TextureSampling` static samplers and the per-bind
     fallback sampler are root-signature objects; unchanged.
   - The mip kernels' typed UAV loads of `float4` need the same
     `TypedUAVLoadAdditionalFormats` support under DXIL as under DXBC;
     `supportsMipGeneration` is unchanged.
3. Flip the default: DXC wherever §4.2 says it can run.
4. CI (§7): the GPU suites run twice on one x64 lane, the second with
   `EACP_D3D12_COMPILER=fxc`, so the fallback stays tested.
5. Docs (§6), first pass.

*Done:* DXC is the default on every Windows lane and green; the FXC re-run
green; the hardware table filled in with a pass/fail per machine and
compiler; any quirk change written up in `SAMPLERS.md` and the README's
"Driver quirks".

### Phase 3 — the wave fast path for the folds

**The shape.** Not `WaveActiveSum`. A shuffle butterfly that reproduces the
existing tree's arithmetic exactly:

```hlsl
// within one wave of W lanes, for step < W:
x = fold(x, WaveReadLaneAt(x, WaveGetLaneIndex() ^ step));
```

For a power-of-two width, the tree at step `s` pairs lane `i < s` with
`i + s`; the butterfly pairs `i` with `i ^ s`, which for `i < s` is the same
`i + s`, and IEEE addition is commutative, so lane 0's value after each step
is bit-identical to the tree's `scratch[0]`, and every other lane of the block
ends with the same bits as lane 0. Max and min are order-free. So the fast
path folds in the fallback's halving order, `CpuCompute` keeps matching it
bit for bit, and the cross-check needs no new tolerance — a better outcome
than Metal's hardware fold, which the tests already tolerate.

**Hybrid on wave width.** `W = WaveGetLaneCount()` is a run-time value
(`[WaveSize]` is SM 6.6, F11) and may be 4 (WARP, F10), 8-16 (Intel, WARP
AVX), 32 or 64. The emitted fold therefore does the tree's steps `≥ W`
through the scratch with barriers, exactly as today, and the steps `< W`
through the butterfly in registers:

- Narrow fold (`simdSum`/`Max`/`Min`, width 32): with `W ≥ 32` every lane
  shuffles within its own 32-block (`i ^ s` for `s ≤ 16` stays in it, also in
  a 64-lane wave) — no scratch, no barrier, every lane holds the answer. With
  `W < 32` the steps 16 … W go through the scratch and the last `log2 W`
  through the butterfly, each wave reading its slice of the scratch.
- Wide fold (`groupSum`/…): steps `≥ W` through the scratch; the remaining
  steps live on lanes `[0, W)`, all in the first wave, which butterflies them;
  lane 0 writes `scratch[0]`, one barrier, everyone reads. Five barriers fewer
  at `W = 32` for any group size.
- Non-power-of-two widths keep today's tree unchanged (compile-time known).

The branch on `W` is uniform across the wave and the decision of how many
scratch steps to run is uniform across the group (every wave has the same
`W` within one dispatch of one pipeline), so the barrier rule holds. WARP's
four lanes exercise the hybrid in CI on every lane, which a `W ≥ 32` gate
would never do.

**Gates.** `EACP_WAVE_OPS` (§4.3) selects the hybrid; FXC devices and
`EACP_D3D12_WAVES=0` take the `#else`, today's text. The hybrid assumes group
lanes `[k·W, (k+1)·W)` share a wave at lane `i mod W` (Phase 0 item 5). If
any vendor breaks that, `compileHlsl` leaves `EACP_WAVE_OPS` undefined for
that adapter, a probe in the `DriverQuirks` style rather than a name check:
a one-off dispatch on the real device on first use of a wave kernel, its
answer cached in `D3D12Shared`.

**Tasks.**
1. `ShaderEmitter.cpp` `groupReduction` (l.2474-2550): the HLSL branch wraps
   today's text as the `#else` and adds the hybrid; a helper per reduction
   kind for the butterfly step; `SV_GroupIndex` is still what indexes the
   scratch. (~120 lines.)
2. `GroupReductionCodegenTests.cpp`: assertions for both branches; the HLSL
   compile checks of Phase 1 compile both (`-D EACP_WAVE_OPS=1` under DXC).
3. `GroupReductionTests.cpp`: on D3D12 with waves on, the float sums agree
   with the CPU half at **zero** tolerance — the test that would fail if
   someone later swapped in `WaveActiveSum`. The existing tolerances stay for
   Metal.
4. Leave `ComputePipeline::threadExecutionWidth()` at 0 on D3D12: the fold
   still folds exactly `simdWidth` logical lanes whatever `W` is, so the
   width warning in `ComputeProgram::prepare` stays correct.
5. *Optional, measured first:* the SIMD-group matrix product
   (`ShaderEmitter.cpp` l.1138-1150 and the product statement) fetching the
   other lanes' pairs with `WaveReadLaneAt` when `W ≥ 32` instead of through
   the scratch and two barriers, keeping the two-floats-per-lane layout so
   the FXC fallback and its register budget are untouched. Results must stay
   bit-identical with `CpuCompute`'s "accumulator plus terms in ascending k,
   unfused" (README l.2659), which fixes the operation order and rules out
   letting DXC contract to `mad` — check `-Gis`/precise is needed.
6. Not in scope: the same hybrid for GLSL through
   `GL_KHR_shader_subgroup_shuffle` and `gl_SubgroupSize` (lavapipe's 8 lanes
   would exercise it as WARP's 4 do). The `#if` shape carries over; it is its
   own plan.

*Done:* both branches green on WARP on all four lanes with zero-tolerance
float sums; a measured speed-up on at least NVIDIA and AMD hardware for
`Tests/GPU/GroupReductionTests` kernels and one real kernel (the
layer-norm-shaped one in README l.1838-1845), recorded here; README updated.

### Phase 4 — fp16 arithmetic in the EDSL (after D3)

Cross-backend, and the largest phase; the D3D12 part is the smallest piece.
Scope under D3 (a):

1. `Codegen/ShaderTypes.h`: `ValueType::Half`, `Half2`, `Half3`, `Half4`;
   `ShaderValue.h`: the handles and their arithmetic, conversions to and from
   `Float` explicit. v1 keeps `Half` out of uniforms and vertex inputs (the
   cbuffer and attribute layouts are their own design) and allows it in
   locals, `shared<Half>` and storage buffers.
2. Emitters: MSL `half` (every Metal device); HLSL `float16_t` under
   `EACP_NATIVE_16BIT`, with the program refusing to build otherwise;
   GLSL `float16_t` with `GL_EXT_shader_explicit_arithmetic_types_float16`
   (+ `GL_EXT_shader_16bit_storage` for buffers).
3. Device queries: `Device::supportsHalfArithmetic()` — Metal true; D3D12 the
   `native16Bit` cap (SM ≥ 6.2 + OPTIONS4, DXC loaded); Vulkan
   `shaderFloat16` and, for buffers, `storageBuffer16BitAccess`, enabled at
   device creation when offered (lavapipe: unverified). `ComputeProgram::
   fitsHalfArithmetic(device)` beside `fitsPackedSimdMatrix`
   (`ComputeProgram.cpp` l.239-249), and `prepare` naming the refusal.
4. `CpuCompute`: `Half` as fp32 storage rounded to binary16 after every
   operation. For `+ − × ÷ √` that is exactly binary16 arithmetic, because
   double rounding through binary32 is innocuous when `24 ≥ 2·11 + 2`
   (Figueroa's condition); transcendentals and anything a GPU computes
   approximately get tolerances in half ulps. Denormal behaviour of fp16 on
   GPUs (flush or keep) is a Phase 0-style survey item.
5. Tests: a `HalfArithmeticTests.cpp` in `GPUTests` with CPU halves; the
   refusal path on an FXC device and on WARP if it lacks the cap.
6. README: replace "fp16 weights, kept packed"'s opening (l.2286-2290) with
   the new rule; `readHalf` and the packed path stay as the portable form.

*Done:* `Half` kernels green against the CPU on Metal, D3D12 (on hardware
with the cap; WARP if it has it) and Vulkan where `shaderFloat16` exists;
refusals tested where it does not.

### Phase 5 — no runtime compiler (optional, later)

Two separate wins; only the second removes the DLL.

1. *A disk cache* of compiled bytecode keyed by a hash of source, entry,
   stage, the predefined symbols, the compiler and its version, under
   `FilePath::appCacheDirectory() / "ShaderCache"`, the D3D12 twin of the
   Vulkan backend's persisted `VkPipelineCache`. Removes compile time from a
   second run; keeps the DLL.
2. *Build-time DXIL.* EDSL graphs are built by C++ at run time, so this needs
   a registry of programs (an `EACP_PRECOMPILE_SHADER(Type)` in the spirit of
   `EACP_RELOADABLE`), a host tool built from the same sources that emits
   each program's HLSL and runs `dxc.exe` at each shader model tier the
   runtime may pick (6.0, 6.2 with 16-bit), and the blobs embedded through
   ResEmbed and looked up by graph hash at `ShaderLibrary` time. A miss
   (hand-written source, a graph shaped by run-time arguments) falls back to
   FXC, so a plugin can ship without `dxcompiler.dll` and lose only the fast
   paths for programs it did not register. Its own plan.

## 6. Documentation

- `Lib/eacp/GPU/README.md`: a "Shader compilers on D3D12" section under the
  D3D12 material (near "Driver quirks", l.3114-3125): DXC by default, the
  FXC fallback and when it is taken, the module-relative DLL, the
  `EACP_D3D12_COMPILER`/`EACP_DXC_PATH`/`EACP_D3D12_WAVES` switches, the
  `EACP_HLSL_SM`/`EACP_WAVE_OPS`/`EACP_NATIVE_16BIT` symbols for hand-written
  HLSL, the 6.5 cap. Then the four FXC statements: l.1660 (the 32 KB figure
  is SM 6.0-6.5's too — reword, not remove), l.1864-1872 and l.1890-1897
  (the reduction paragraphs; Phase 3 rewrites them), l.2004-2027 (the
  register-budget story stays true of the fallback; say so), l.2286-2290
  (Phase 4). l.327 stays: start-vertex/instance system values are SM 6.8.
- `Lib/eacp/GPU/SAMPLERS.md` l.175-178: the root signature stays SM 5.0
  compatible because DXBC (bgfx's, and the FXC fallback's) binds against it;
  plus any Phase 2 quirk finding.
- `Docs/Build.md`: DXC in "Dependencies" (l.14-36: fetched on Windows by
  CPM, pinned version and hash, `EACP_D3D12_DXC`, the DLL copied beside
  every target that links `eacp-gpu`, `eacp_deploy_gpu_runtime` for a plugin
  project's own targets and bundle layout); the 32 MB download in "Continuous
  integration" (l.376-416, next to the uncached-CPM note at l.414-416); the
  FXC re-run.
- `CLAUDE.md`: no line names FXC or a shader model today. Add DXC to the
  dependencies paragraph (Windows-only, pinned, runtime DLL beside the
  module), to "Framework Dependencies" (Windows), and one sentence to the
  GPU coverage text that D3D12 compiles through DXC at the device's shader
  model with FXC as the fallback.
- `README.md` (the user-facing one): one line that a Windows app or plugin
  using the GPU module ships `dxcompiler.dll` beside its binary, and what
  happens without it. No build detail.

## 7. CI

- No new lanes. The four Windows lanes fetch DXC at configure (32 MB from
  GitHub's release CDN, uncached like the rest of CPM's sources; an
  `actions/cache` keyed on the DXC version is a cheap addition if the
  download shows in the timings).
- Phase 1: one x64 lane runs `ctest -R '^(GPU|UI|GPUWidgets)'` a second time
  with `EACP_D3D12_COMPILER=dxc`. Phase 2 inverts it: DXC is the default
  everywhere and the second run is `fxc`.
- The `describeAdapter` line puts the shader model, wave lanes and 16-bit
  cap of every runner's WARP in the log, which keeps F9/F10 current for free.
- Optional: the redistributable WARP (F9) on one lane, copied beside the test
  binaries, for SM 6.2+ and wider waves than the inbox WARP has — only if
  Phase 0 shows the inbox one is below what Phase 3 or 4 needs.
- ccache is unaffected: nothing new is compiled at build time until Phase 5.

## 8. Risks

- **Driver DXIL compilers are a different code path** from their DXBC ones,
  on every vendor. The Phase 2 hardware pass is the only defence; WARP in CI
  is the reference, not the hardware. Dawn's Intel Gen11 "no SM 6.6"
  workaround (search results above) is the kind of thing to expect.
- **Lane-to-wave mapping** is implementation-defined; Phase 3 assumes the
  linear mapping every vendor is believed to use. Mitigated by the Phase 0
  survey and the per-adapter probe; the failure is a wrong fold, not a crash,
  so the probe is not optional.
- **Wave size varies per pipeline** (AMD wave32/64, Intel 8/16/32) and the
  reported minimum can lie (F11). The hybrid reads `WaveGetLaneCount()` per
  dispatch rather than trusting `OPTIONS1`, which is why it is shaped that
  way.
- **Inbox WARP wave bugs** (F9): the CI device may itself compute some wave
  ops wrongly. The butterfly uses `WaveReadLaneAt` and `WaveGetLaneIndex`/
  `Count` only; if WARP gets even those wrong, CI needs the redistributable
  WARP.
- **Numeric drift in existing tests** from denormal mode and from integer
  edge cases DXIL leaves undefined (Phase 2 item 1).
- **22 MB per binary** in every app and plugin, and a deployment step every
  plugin project has to get right; mitigated by the FXC fallback (D1) and,
  eventually, Phase 5.
- **Host processes.** A DAW that has loaded its own `dxcompiler.dll` by name
  does not affect a full-path load (Phase 0 item 4 confirms); a DAW that
  loaded the Agility SDK runs a newer runtime in-process and may report a
  higher shader model, which the 6.5 cap makes harmless.
- **First-compile latency**: loading 22 MB and creating the compiler on the
  first shader; measured in Phase 0, and it lands on the same lazy path that
  compiles the first pipeline today.
- **A DXC upgrade changes generated code** everywhere at once; pinned by
  version and hash, `-HV 2021` explicit, and upgraded as a reviewed change.

## 9. Open questions

1. D1 — keep the FXC fallback (recommended) or require SM 6.0?
2. D3 — `Half` as exact binary16 that refuses to build without the cap
   (recommended), or relaxed `min16float` semantics; and is reversing the
   README's "there is not going to be one" wanted at all?
3. The shader-model cap: hold at 6.5 so Windows 10 and 11 run the same code,
   or compile at whatever the device reports?
4. `Device::shaderModel()` as a public query — worth having for hand-written
   HLSL users, or keep the `EACP_HLSL_SM` symbol as the only interface?
5. Does the in-tree DEFER walk for deployment suffice, or should every
   executable that links `eacp-gpu` be required to call
   `eacp_deploy_gpu_runtime` explicitly (configure error instead of warning)?
6. Is the Linux `libdxcompiler.so` compile check on the Linux lanes (Phase 1
   item 9) worth a 13 MB fetch there?
7. Phase 5: is a disk cache enough, or is shipping without the DLL a goal?
