# OpenGL backend — plan

Written 2026-09-17 against `80b93a9a` (branch `opengl-backend`), from a read
of the Vulkan backend, the shader emitter, the GPU test harness, the UI's
coverage atlas and a probe of the development VM's drivers. Line counts are
estimates, not commitments.

## Progress

| Stage | State | Notes |
| --- | --- | --- |
| 0 — runtime backend seam | done | Landed against `1c465650`. Headless (`EACP_HEADLESS=1 EACP_VK_SOFTWARE=1 EACP_REQUIRE_GPU=1`): `GPUTests` 490 -> 491, `GPUWidgetsTests` 57, `UITests` 183 -> 184; 720 -> 722 cases, all passing. Under `with-weston`, the same 722 and the 10 `Present/` cases; under `with-xvfb`, the same 10 `Present/` cases. The two new ones are `GPU/computeIsSupportedAndCanBeTakenAway` and `ComponentHost/aHostWithoutComputeMeshesEveryPath`. |
| 1 — GLSL lowering | done | Landed beside stage 0. `Codegen/GlslLowering.{h,cpp}` and `Spirv::validateGlsl`; `GlslLoweringTests` 13 cases in `GPUCodegenTests` (104 -> 117), and every source the suite compiles for Vulkan is now lowered and validated for the four targets as well. No device involved. |
| 2 — GL render, headless | done | `eacp-gl` over a committed glad2 loader (`ThirdParty/glad`, `CMake/FindGLBackend.cmake`), `GPU/OpenGL/` and one `-GL.cpp` per class: `Device`, `Buffer`, `Texture`, `ShaderLibrary`, `GpuTimestamps`, `RenderPipeline`, `Frame`, `RenderPass`, `CommandBuffer`, a `ComputePipeline` that refuses (D9, stage 6) and a headless `GPUView` (stage 3). Headless Vulkan unchanged: `GPUTests` 495, `GPUWidgetsTests` 57, `UITests` 184, `GPUCodegenTests` 117, all passing. Headless GL on llvmpipe core 4.5 (`EACP_HEADLESS=1 EACP_GPU_BACKEND=gl EACP_REQUIRE_GPU=1 LIBGL_ALWAYS_SOFTWARE=1`): the same four counts, all passing, with 146 of `GPUTests`' cases self-skipping on `supportsCompute()` and the kernel half of `GPUWidgetsTests` skipping with them. The same four on llvmpipe's ES 3.2 (`EACP_GL_ES=1`) but for two Mesa crashes, and on virgl core 4.0 but for four cases - both listed under stage 2 below. |
| 3 — GL present | done | `GPUView-GL.cpp` is an `EGLSurface` over the view's `wl_egl_window` or its X11 child window, with the display opened on the window system's own platform. Headless is unchanged on both backends: `GPUTests` 496 (495 + the new present case), `GPUWidgetsTests` 57, `UITests` 184, `GPUCodegenTests` 117, all passing. Under `with-weston` the 11 `Present/` cases pass on Vulkan and on GL; under `with-xvfb` the 11 `Present/` and 17 `EmbeddedView/` cases pass on both, and on GL's ES profile as well. `Wayland/` (13) and `X11/` (47) unchanged. The new case is `Present/whatIsPresentedIsOnTheWindow`, which reads a pixel back off the window. |
| 4 — selection and the dev VM | done | D8's `auto` rule, the two capability queries the virgl failures needed, the five GL steps on the CI Vulkan lane, the `Dockerfile` and the three READMEs. Headless (`EACP_HEADLESS=1 EACP_REQUIRE_GPU=1`): `GPUTests` 496 -> 502, `GPUWidgetsTests` 57, `UITests` 184, `GPUCodegenTests` 117, all passing on Vulkan and on GL alike - llvmpipe core 4.5, llvmpipe capped to core 3.3, llvmpipe capped to ES 3.0, and **virgl core 4.0, now clean at 501 of 501** (the whole of it less the one case that wedges the guest). The six new cases are `Backend/`, which pin the rule over every pair of probe answers. Windowed: 1820 under `with-weston` and 75 under `with-xvfb`, on both backends. |
| 5 — composite device | done | D11 as a third `DeviceBackend` over the other two (`GPU/Linux/CompositeBackend-Linux.{h,cpp}`, 1,449 lines), the `auto` rule's third fact, the three crossing counters on `Device`, the CI steps and the four READMEs. Headless (`EACP_HEADLESS=1 EACP_REQUIRE_GPU=1`): `GPUTests` 502 -> 509, `GPUWidgetsTests` 57, `UITests` 184, `GPUCodegenTests` 117, all passing on Vulkan, on GL and on the **composite over two llvmpipes** - where the compute half of the suite stops self-skipping, so it is the first GL-side run that dispatches a kernel. On the VM: **virgl render, llvmpipe compute, 508 of 508** (the whole of it less the one case that wedges the guest), and `auto` chooses it here - `GPU: auto chose OpenGL+Vulkan (Vulkan: software, OpenGL: hardware)`. Windowed: 1827 under `with-weston` and 75 under `with-xvfb` on Vulkan and GL alike, and the 24 `Wayland|Present` and 75 `X11|EmbeddedView|Present` cases on the composite. The seven new cases are six `Crossing/` ones and one `Backend/`. |
| 6 — GL compute tier | pending | Optional, not started: this VM never reaches a GL 4.3 compute stage and the stage 5 composite covers the machines that do not either. What it would flip is recorded under D8 (stage 2 as built) and D9. |

Where the code differs from the sketches below, the code wins; each such
point is marked *as built* in place.

## 0. Why

The Vulkan backend is the whole of Linux graphics, and a Linux guest in a
virtual machine commonly has no Vulkan device but a good OpenGL one. The
development VM is the concrete case: an arm64 Ubuntu guest under Parallels on
an Apple Silicon Mac, whose virtio-gpu speaks virgl (OpenGL forwarded to the
host) and not Venus (Vulkan forwarded to the host). Probed on 2026-09-17:

| Asked for | Answer |
| --- | --- |
| Vulkan | `llvmpipe`, `PHYSICAL_DEVICE_TYPE_CPU`, Mesa 26.0.8 |
| OpenGL 4.6 / 4.3 core | refused |
| OpenGL 3.3 core | `virgl (Apple M5 Max (Compat))`, reports GL 4.0 core, GLSL 4.00 |
| OpenGL ES 3.2 / 3.1 | refused |
| OpenGL ES 3.0 | virgl, GLSL ES 3.00 |

So every frame on this machine renders on the CPU while a hardware GL sits
unused. The same shape — software Vulkan, hardware GL — is what VMware,
VirtualBox and older QEMU guests offer, and what a Linux box with a driver
stack that predates Vulkan offers. An OpenGL backend beside the Vulkan one,
chosen at runtime, puts those machines on the GPU.

Two things this plan is deliberately *not* for. It is not the Android
backend: Vulkan 1.1 has been mandatory on 64-bit Android since Android 10
and Android's own GLES now runs through ANGLE over Vulkan, so the Android
backend is the Vulkan backend plus an `ANativeWindow` surface kind, which is
a separate plan. And it is not a fourth shader dialect: the tree keeps one
GLSL (§2, D3).

The floor is OpenGL 3.3 core and OpenGL ES 3.0, because that is what the
VM offers and because everything below it (GL 2.1, ES 2.0: no uniform
blocks, no instancing, no sync objects) would need a different uniform model
from the one every eacp shader assumes.

## 1. What is there and what is missing

**The backend seam is compile-time.** Every public GPU class is
`struct Native; Pimpl<Native> impl;` with exactly one definition of `Native`
per platform, selected by which `-Apple.mm`, `-Windows.cpp` or `-Linux.cpp`
files `Lib/eacp/GPU/CMakeLists.txt` compiles. Thirteen `-Linux.cpp` files
define the Vulkan backend (about 7,000 lines, plus `VulkanContext.h` and the
`Spirv/` compiler; the D3D12 backend is 7,400, Metal 4,100). Cross-class
access goes through `void* nativeX()` handles that each backend casts to its
own types inside its own files — `RenderPass-Linux.cpp` casts
`Buffer::nativeBuffer()` to a `VulkanBufferData*` and nothing outside the
Vulkan files ever sees that type. `Pimpl<T>` constructs `T` itself
(`std::make_shared<T>(args...)`), so it cannot today hold a derived
`Native`.

**Device selection is inside the Vulkan context.** `Device::Native` is one
`VulkanContext`; `Device::isValid()` is `context.isValid()`, false when no
`VkDevice` came up, and every GPU test self-skips on that (a bare `return`
after `if (!Device::shared().isValid())`). `EACP_VK_SOFTWARE` inverts the
physical-device preference; `EACP_REQUIRE_GPU` turns the skip into a
failure. There is no notion of "which backend" anywhere — a `Device` is a
Vulkan device because this is Linux.

**A `Device` is single-threaded**, owned by the thread that constructed it,
with `Device::shared()` bound to the main thread and use off the owning
thread a debug assertion (`Device.h`). This is exactly the constraint an
OpenGL context has, so the GL backend needs no context-sharing machinery:
one context per `Device`, current on its thread.

**The shader emitter has three dialects** (`Codegen/ShaderEmitter.cpp`:
`Backend::Metal|DirectX|Vulkan`, 27 Vulkan-specific decision points) and
emits Vulkan GLSL 450: `#version 450`, `layout(std140, set = 0, binding =
N)` uniform blocks, `layout(std430, set = 0, binding = N)` storage buffers,
`layout(set = 0, binding = N) uniform sampler2D`, `writeonly image2D` with
no format qualifier, `layout(location = i)` on attributes *and* varyings,
no push constants, no `gl_VertexIndex`/`gl_InstanceIndex`, and reductions
through shared memory rather than the subgroup extension (which the emitter
already refuses to assume, "unassumable and the wrong width"). The binding
numbers come from `Codegen/ShaderBindings.h`. `ShaderBuilder-Linux.cpp`
answers `nativeShaderSource()` with `ShaderSource::glsl(emitGlsl(graph))`,
which tags the source `ShaderBackend::Vulkan`. Tests carry hand-written
twins per dialect (`Tests/GPU/Common.h`, `nativeDialect(msl, hlsl, glsl)`,
picked by platform), and every GLSL twin and every emitted GLSL source is
compiled by glslang (`Spirv::compileGlsl`, `EShClientVulkan`,
`EShTargetVulkan_1_3`) wherever `eacp-spirv` is built, which is every Linux
lane.

**What GL 3.3 / ES 3.0 can carry** of the render half: vertex and fragment
stages, one std140 block per stage bound with `glBindBufferRange`, vertex
buffers in a VAO, indexed and instanced draws, the eleven `TextureFormat`s
(BC1/2/3/7 through `EXT_texture_compression_s3tc`/`ARB_texture_compression_bptc`
where offered), cube maps, mip chains, MSAA with a blit resolve, depth and
stencil with `StencilFace` ops and a reference, scissor, viewport, the
`BlendState` table, cull and winding, `ARB_timer_query` /
`EXT_disjoint_timer_query`, `ARB_sync` fences, read-back through an FBO and
`glReadPixels`. What it cannot: `setVertexStorageBuffer` /
`setFragmentStorageBuffer` (SSBOs are 4.3 / ES 3.1; nothing in `UI`,
`Sprites` or `GPUWidgets` calls them) and the whole compute half.

**The compute half needs GL 4.3 / GLES 3.1**: kernels, storage buffers,
`imageStore`, atomics, `shared` arrays with `barrier()`, indirect dispatch.
The SIMD-group matrix and the packed fp16/bf16/int8 weights have no OpenGL
equivalent at any version, so `supportsHalfSimdMatrix()` and its siblings
answer false there.

**The UI needs compute.** `UI::ComponentHost` gathers every dirty vector
shape into one `GPUWidgets::CoverageBatch` and records one
`frame.beginCompute()` dispatch per frame — the four binning kernels, the
prefix sum and the coverage kernel of `PathRasterizer` — into the R8
`CoverageAtlas`. Glyphs, rectangles and gradients need nothing from
compute. `PathShape::buildMesh` is a triangle route already in the tree,
taken today only for shapes too large for the atlas; there is no
"no compute" switch that forces it.

**What the tree already does that this plan copies.** The Vulkan loader is
`dlopen`ed by `volkInitialize()` so a machine with no driver builds the same
binary and reports `isValid()` false; `CMake/FindVulkanBackend.cmake` is one
`eacp-vulkan` target holding fetched headers and `volk.c`; the D3D12
`DriverQuirks` finds a driver's gaps by trying, never by name; `Scripts/
with-weston` and `with-xvfb` give the present tests a real window system on
CI; `Tests/GPU/CMakeLists.txt` locks the display-bound cases with
`RESOURCE_LOCK`.

## 2. Decisions

**D1 — The Linux backend is chosen at runtime, behind the existing pimpl.**
Every `-Linux.cpp` becomes the *forwarding* file for its class and the
Vulkan body moves to a `-Vulkan.cpp` beside it. A new
`GPU/Linux/GPUBackend-Linux.h` declares one abstract struct per class —
`DeviceBackend`, `BufferBackend`, `TextureBackend`, `ShaderLibraryBackend`,
`RenderPipelineBackend`, `ComputePipelineBackend`, `FrameBackend`,
`RenderPassBackend`, `ComputePassBackend`, `CommandBufferBackend`,
`GpuTimestampsBackend`, `GPUViewBackend` — whose virtuals are the public
methods of the class, one to one. `X::Native` on Linux is
`std::unique_ptr<XBackend> backend` and nothing else; the public method
bodies in `X-Linux.cpp` are one line each. The backend that made the
`Device` makes every object under it (`DeviceBackend::makeBuffer(...)` and
so on), so no object ever asks which backend it is on. `void* nativeX()`
keeps returning the backend's own handle, and every cast of one stays inside
that backend's files, exactly as today. `Pimpl<T>` gains a constructor from
`std::shared_ptr<T>` (or the Linux `Native` structs hold the pointer
themselves; either way `Core/Utils/Pimpl.h` is touched at most once).

*As built.* The second of those: every Linux `X::Native` is
`std::unique_ptr<XBackend> backend` and `Core/Utils/Pimpl.h` is untouched.
Four more things the sketch did not say:

- A `Device`'s backend is reached through `getDeviceBackend(device)`
  (`GPUBackend-Linux.h`), which is `Device::nativeContext()` cast: on Linux
  that handle is now the `DeviceBackend` itself and the backend's own
  per-Device state is one virtual further in
  (`DeviceBackend::nativeContext()`, which is what `getVulkanContext` reads).
  So no new accessor on `Device.h`.
- The per-class Vulkan factories (`makeVulkanBuffer` and the rest) are
  declared in a header of their own, `GPU/Vulkan/VulkanBackend-Linux.h`, so no
  `-Vulkan.cpp` includes another.
- A pass is made by whatever opened the recording rather than by the Device:
  `FrameBackend::beginPass` and `CommandBufferBackend::beginCompute` return
  the pass backend, and the `void*` the public `RenderPass`/`ComputePass`
  constructor takes is that pointer, adopted. `ComputePassBackend::dispatch`
  is one call taking the three extents and the group, since the public
  overloads differ only in what they clamp; `setPipeline` answers whether it
  bound, which is what the portable half drops a dispatch under.
  `RenderPassBackend` has one `setStorageBuffer` and one `setBytes` for the
  vertex and fragment pairs, which is what the backend already did.
- `Buffer::canAdoptMemory` and `Buffer::memoryPageSize` stay in the forwarder:
  they are the platform's answer rather than a backend's, and no Linux backend
  adopts host memory.

The virtual call per public method is noise against the API call behind it.
Metal and D3D12 are untouched: their `Native` structs stay concrete, and the
interface header is compiled on Linux only.

`GPUView-Linux.cpp` is the one file that is not a pure forwarder. Its
pacing — `startContinuous`, `tickIsDue`, `setMaxFps`'s divider, the
`ViewSurface` hooks, `frameCallbackArrived`, the off-screen
`renderNativeContent` path — is window-system logic that neither API owns,
and it stays there. What moves behind `GPUViewBackend` is
`createSurface`/`destroySurface`, the swapchain and its companions,
`renderOneFrame` and `handlePresentResult`.

*As built.* The backend is made in `GPUView::Native`'s constructor from
`Device::shared()`, so constructing a `GPUView` is now what brings the shared
device up rather than the first frame that needs companions. `noteDeviceLost`
reports through a `GPUViewBackend::onDeviceLost` callback the pacing half sets
to `stopContinuous`, since stopping the tick is the pacing half's business.

**D2 — One `Device`, one context, one thread.** The GL `DeviceBackend` owns
one `EGLDisplay` (process-wide, refcounted, like `VulkanShared`), one
`EGLContext` and, headless, no surface at all (`EGL_KHR_surfaceless_context`,
which Mesa has had for a decade; `EGL_MESA_platform_surfaceless` where
neither a Wayland nor an X11 display is reachable). The context is made
current on the owning thread at construction. Two `Device`s on one thread
are two contexts; a thread-local "current backend" pointer makes
`eglMakeCurrent` happen only when the device changes, and a `Device` used
off its thread is already a debug assertion above this layer. Resources are
per-`Device`, as on every backend.

*As built (stage 3).* The display is not "the default one unless headless": it
is opened on the **platform of the window system this copy will present to**,
because a window surface can only be made on a display of its own platform and
the display is opened when the `Device` comes up, long before any view has a
surface to read one off. See stage 3's note below. The thread-local compare
gained the context's current surface beside it, `makeCurrent()` keeping whatever
surface the context has (an off-screen frame draws through an FBO and cares
about none) and `makeCurrentOn(surface)` binding a view's own.

*As built (stage 2).* As sketched, with three things the sketch did not say:

- **The version is a descending ladder, not a request.** `eglCreateContext` is
  asked for 4.6, 4.5, 4.3, 4.1, 4.0 then 3.3 core (3.2, 3.1, 3.0 under
  `EACP_GL_ES=1`) and the first that answers is taken, because a driver
  refuses a version it cannot give rather than downgrading - virgl refuses
  4.6 and 4.3 and answers the 3.3 request with a GL 4.0 context.
  `GLCapabilities::version` is what the context reported, not what was asked.
- **`followMainThread()` releases the context.** An EGLContext is current on
  one thread at a time, and `Device::shared()` is constructed by whichever
  thread asked for it first and driven by the main one afterwards; the
  release is what lets the main thread's first use take it. Everything else
  is the thread-local compare - `GLContext::makeCurrent()` at the top of every
  operation, a no-op when this thread already has this context.
- **The display is refcounted with a probe beside it.** `eglTerminate` runs
  when the last `Device` goes, but the display a question like D8's `auto`
  rule opens without making a `Device` is kept up and joined by the next one,
  rather than terminated and opened again.

**D3 — One GLSL, lowered.** The emitter keeps emitting Vulkan GLSL 450 and
the tests keep one hand-written GLSL twin. A new portable pass in
`eacp-gpu-codegen`, `Codegen/GlslLowering.{h,cpp}`, turns that dialect into
a target:

```cpp
struct GlslTarget
{
    enum class Profile { Core, ES };
    Profile profile = Profile::Core;
    int version = 330;           // 330, 400, 430, 460; 300, 310, 320 for ES
};

struct LoweredGlsl
{
    std::string source;
    // Block or sampler name -> the binding the Vulkan source named, for the
    // versions whose layout() cannot say it (core < 420, ES < 310): the GL
    // ShaderLibrary binds these by name after linking.
    Vector<NamedBinding> bindings;
};

LoweredGlsl lowerGlsl(std::string_view vulkanGlsl, GlslTarget target,
                      ShaderStage stage);
```

The rules are few and each is a test: the `#version` line is rewritten
(`450` → `330 core`, `300 es` and so on) and ES gets a
`precision highp float; precision highp int;` line; `set = 0, ` is removed
everywhere; `binding = N` is kept where the target allows it and otherwise
removed and recorded; `layout(location = i)` is kept on vertex inputs and
fragment outputs (3.3-legal) and removed from varyings (4.1 /
`ARB_separate_shader_objects`; the emitter names them `varyN` and hand-written
twins match by name too, so the interface matches without it); `writeonly
image2D` is unchanged (legal without a format when never read). No other
construct in the emitted sources or the twins is Vulkan-only — there is no
push constant, no `gl_VertexIndex`, no subgroup call. A source the pass
does not recognise fails loudly at pipeline creation, not silently at draw.

`Spirv::compileGlsl` gains a target: the codegen tests run every emitted
source and every twin through `lowerGlsl` for `{Core 330, Core 430, ES 300,
ES 310}` and ask glslang to validate each against the OpenGL client
(`EShClientOpenGL`; `EEsProfile` for ES) so a lowering regression, or an
emitter change that leaks a Vulkan-only construct, fails on every Linux
lane with no GL device at all — the same guarantee the Vulkan dialect has
today. `ShaderBackend::Vulkan` is left as the tag for "eacp's GLSL"; the GL
`ShaderLibraryBackend` accepts it and lowers.

*As built (stage 1).* The pass is line-based text in and text out, and it
answers with more than the source. It also **defines the stage macro**
(`#define EACP_VERTEX 1` / `EACP_FRAGMENT`) under the rewritten `#version`,
because a render source is two stages behind one pair of `#ifdef`s and what a
GL `glShaderSource` takes is one: the result is a standalone single-stage
source, compiled with no preamble. It **records every binding** in
`LoweredGlsl::bindings` whatever the target let the `layout()` keep, since D4
binds by name after linking anyway, and a complete list costs nothing.
Failure is two things, not one: `error` alone is a source the pass does not
recognise (a push constant, `gl_VertexIndex`, a `subgroup*` call, an
`#extension`, a `#version` that is not 450, a vertex stage that does not spell
its entry `void main()`), while `needsNewerTarget` beside it is a source the
*target* is too old for — a kernel or a storage buffer below core 430 / ES 310,
an image below core 420 / ES 310, `packHalf2x16` below core 420 — which is
"this device cannot run this shader", and the four-target test check skips
rather than fails on it. Two rewrites beyond the rules above turned out to be
needed, both found by glslang and both unavoidable on the floor: ES declares no
default precision for an image, so a source declaring one gets
`precision highp image2D;` beside the two the rules name; and a brace
initializer is core 420 and has never been ES, so the emitter's
`vec3 a0[4] = {...}` becomes the constructor form `vec3 a0[4] = vec3[4](...)`
every version reads.

The validation is `Spirv::validateGlsl(stage, source, target)` — its own entry
point returning a `ValidationResult` rather than an overload of `compileGlsl`,
because OpenGL compiles GLSL in the driver and there is no SPIR-V to hand back
— and it runs glslang's **plain GLSL front end** at the target's version and
profile, not `EShClientOpenGL`. That client means "GLSL compiled to SPIR-V for
GL" (`ARB_gl_spirv`), whose extra rules include a location on every default
uniform, which would reject D7's own `eacpClipYSign`. `eacp-spirv` links
`eacp-gpu-codegen` for the `GlslTarget`, and `Spirv/` is added after that
target in `GPU/CMakeLists.txt`. The four-target check sits in
`Tests/GPU/CodegenCommon.h`'s `expectStageCompiles`, so one place covers the
emitted sources of `GPUCodegenTests`, the hand-written twins of `GPUTests` and
`UITests`' module shaders; it also fails a source no target carried at all, so
a pass that refused everything could not pass as green.

**D4 — One GL backend for every version, capabilities not versions.** The
GL backend is one set of `-GL.cpp` files. `GPU/OpenGL/GLCapabilities.h` is
filled once at context creation — profile, version, and a flag per feature
the code branches on: `computeShaders`, `storageBuffers`,
`imageLoadStore`, `bufferStorage` (persistent mapping), `textureStorage`,
`timerQuery` (and whether it is the disjoint ES form), `explicitBindings`,
`separateShaderObjects`, `bptc`, `s3tc`, `clipControl`, `debugOutput`. No
line below device creation compares a version number. The floor path is
the default everywhere and the higher-version forms are optional speed
behind a flag: `glBufferSubData` always, persistent mapping for
`BufferStorage::Streaming` when `bufferStorage`; `glVertexAttribPointer`
in one VAO per device always; link-time binding by name always (D3 makes it
free); the FBO-and-`glReadPixels` read-back on both profiles rather than
`glGetTexImage`. The ES differences that are not additive live in one
place, a format table: no `GL_BGRA` internal format on ES, so `BGRA8Unorm`
is stored RGBA with a swizzle (`GL_TEXTURE_SWIZZLE_*`, core in ES 3.0) and
uploaded through a per-row swap; BC formats behind their extension flags,
refused at creation where absent, as the Vulkan backend refuses a
multisampled sampleable depth it cannot resolve.

`EACP_GL_ES=1` binds `EGL_OPENGL_ES_API` instead of `EGL_OPENGL_API`, so
the ES path is a run of the same binary, and `MESA_GL_VERSION_OVERRIDE` /
`MESA_GLES_VERSION_OVERRIDE` / `MESA_EXTENSION_OVERRIDE` cap what llvmpipe
reports, so the floor, the compute tier and both profiles are all runs of
one driver on one CI lane (§4, stage 4).

*As built (stage 2).* The flag set is the one D4 names plus six the code
turned out to branch on, each of them a spelling difference rather than a
feature: `bufferStorageIsEXT` and `timerQueryIsEXT` (the ES forms of those
two are separate entry points, not the same ones under another name),
`getTexImage` and `getBufferSubData` (desktop-only reads, with the map-read
and the FBO as the ES routes), and `floatRenderTargets` /
`halfFloatRenderTargets` (on ES a float colour attachment needs
`EXT_color_buffer_float`, so a float render target is refused at creation
where it is absent, exactly as a BC format is). `bgraFormat` is desktop core
alone: ES's `EXT_texture_format_BGRA8888` is an *unsized* `GL_BGRA_EXT`
internal format and not the pair a sized `GL_RGBA8` texture is uploaded
through, so it does not answer the question and the swizzle route is always
taken there.

One divergence from the sketch's "the FBO-and-`glReadPixels` read-back on both
profiles rather than `glGetTexImage`": the FBO **is** the path on both, and
`glGetTexImage` is the fallback for the one case the FBO cannot serve - a
texture in a format this context cannot attach, which on desktop still reads
back and on ES does not. The FBO is made lazily on the first read or pass and
dropped if it comes back incomplete, so the fallback is chosen by asking
rather than by comparing a version.

**D5 — Nothing links libEGL or libGL.** `CMake/FindGLBackend.cmake` is
`FindVulkanBackend.cmake`'s twin: one `eacp-gl` target holding a glad2
loader generated once for EGL 1.5 plus GL 3.3–4.6 core plus GLES 3.0–3.2
with the extensions D4 names, committed under `ThirdParty/glad` beside its
license the way miniz is, its own C target so it never joins a unity build.
glad's EGL loader takes our own `dlopen("libEGL.so.1")` opener and the GL
entry points come from `eglGetProcAddress`, so a machine with no EGL builds
the same binary and `Device::isValid()` is false, exactly the Vulkan rule.
The three `wl_egl_window` functions are declared locally and taken from
`dlopen("libwayland-egl.so.1")` for the same reason. The build needs no
package it does not already need; CI and the `Dockerfile` add only the
runtime `libegl1 libgles2 libgl1-mesa-dri` (Mesa's llvmpipe GL) to run the
suite on it.

*As built (stage 2).* glad2 2.0.8, generated with no internal loader at all
(`--loader` absent), so `gladLoadGL`/`gladLoadGLES2`/`gladLoadEGL` each take
the loader function we hand them and glad opens nothing itself. One merged
`glad/gl.h` carries both profiles (`--api='gl:core=4.6,gles2=3.2,egl=1.5'
--merge`). `EGL_MESA_device_software`, which D5's extension list names, is
**not** generated: the Khronos registry has no XML for it and it defines no
entry point or token - a software device is the string appearing in
`eglQueryDeviceStringEXT(device, EGL_EXTENSIONS)`, which needs nothing
generated. The glad sources are added to `eacp-gl` from
`CMake/FindGLBackend.cmake` rather than from `ThirdParty/CMakeLists.txt`,
because that file is added on every platform while nothing outside the Linux
GL backend links them; `ThirdParty/glad/README.md` names the version, the
command and the list.

*As built (stage 3).* The `wl_egl_window` half is as sketched: three functions
declared locally in `GPUView-GL.cpp` and taken out of a `dlopen`ed
`libwayland-egl.so.1`, so nothing links it either. One thing about the loader
that stage 2 did not have to notice: glad reads the EGL version off a *display*,
and the first load has none, so `GLAD_EGL_VERSION_1_5` is 0 there and the core
`eglGetPlatformDisplay` is left null. The platform display is therefore opened
through `eglGetPlatformDisplayEXT` (`EGL_EXT_platform_base`, a client extension
glad does load with no display), with the core call as the fallback rather than
the other way round - and the two attribute lists differ in type, `EGLint`
against `EGLAttrib`, which is why one small helper takes the screen number
instead of a list. Everything else, the core 1.5 entry points included, is
loaded by the second pass once the display is initialized.

**D6 — The render mapping.** A `RenderPipeline` is one linked program plus a
plain struct of the descriptor's state (blend, mask, depth, stencil, cull,
winding, topology, sample count) applied in `setPipeline` by diffing against
the pass's last-applied struct, so a pipeline switch costs only the calls
that change. The vertex layout is applied at draw through the device's one
VAO when the bound buffers or the layout differ from the last draw. The
uniform ring is one UBO per frame slot written with `glBufferSubData` and
bound with `glBindBufferRange` at `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` (the
GL twin of `storageBufferOffsetAlignment`; `Device` reports it). A render
target `Texture` carries a lazily made FBO; a multisampled one draws into a
renderbuffer companion and resolves with `glBlitFramebuffer` at pass end, so
the texture always holds the resolved picture as on the other three
backends; depth and stencil are a `GL_DEPTH24_STENCIL8` renderbuffer, or a
depth texture when `sampleableDepth`. A `Frame` is the FBO it targets, and
`flush()` is `glFlush` plus a fence; `CommandBuffer::commit` is a fence
wait, as its contract already says it blocks. `GpuTimestamps` is
`GL_TIMESTAMP` queries in the same slot layout, read when
`endSlot`'s frame retires. The "every recording ends with a global memory
barrier" rule of the Vulkan backend has no render-only equivalent and needs
none; the compute tier (D9) adds `glMemoryBarrier` at the same points.

*As built (stage 2, the resource half of D6).* Four things worth pinning
before the render half:

- **`GLTypes.h` is the seam.** `GLBufferData`, `GLTextureData` and
  `GLShaderLibraryData` are what the `void* nativeX()` handles point at;
  `GLPipelineState`, `GLResolvedBinding`, `GLRenderPipelineData` and
  `GLTimestampSlot` are declared there too so the render half inherits them
  rather than inventing them, and `GLFrameData`, `GLRenderEncoder` and
  `GLCommandEncoder` are defined there beside them by that half.
- **A `ShaderLibrary` holds compiled shader objects, not a program.** The
  pipeline links its own, because the pipeline is what decides the vertex
  layout and the state around it, and the library carries the lowering's
  `bindings` list beside them for the link-time bind by name.
- **`Device::storageBufferOffsetAlignment()` is one number for both binds**:
  the larger of `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` and, where there are
  storage buffers, `GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT`. A caller that
  rounds to it satisfies the uniform ring and a kernel's storage range alike,
  so no `uniformBufferOffsetAlignment` was added to `Device.h` - a second
  query would only be the smaller of the two under another name, and a caller
  cannot use it without knowing which bind it is about to make. It is 16 on
  llvmpipe and 256 on virgl.
- **Nothing flips anything in the resource half.** eacp's row 0 is GL's y = 0,
  so an upload goes up in image order, a region's y is GL's y, and a read-back
  of a target that was rendered into comes out top row first - the whole of
  the flip is D7's vertex wrapper, which the render half sets the sign for.
- **A render target's depth is a `GL_DEPTH24_STENCIL8` renderbuffer**
  (`GL_DEPTH_COMPONENT24` with no stencil plane), or a depth *texture* where
  `sampleableDepth` asked for one; a multisampled sampleable-depth target gets
  the renderbuffer as the attachment and a texture beside it for the resolve,
  which is the Vulkan shape.

*As built (stage 2, the render half of D6).* As sketched - one program per
pipeline, the state struct applied by diffing, the layout applied at draw
through the device's one VAO, one UBO written with `glBufferSubData` and bound
with `glBindBufferRange`, the FBO per target with its blit resolve, `flush()`
as `glFlush` plus a fence, `CommandBuffer::commit` as a fence wait, and the
`GL_TIMESTAMP` queries in the Vulkan slot layout. Seven things the sketch did
not say:

- **`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` where the context has it**,
  set once beside the context. Only the *depth* half of it: the y flip stays
  D7's wrapper, because a context without clip control still has to be right
  and one rule is better than two. What it buys is the [0, 1] clip depth the
  other three backends have, so a depth value read back off a target is the
  number the vertex stage wrote. Core 4.5, `GL_EXT_clip_control` on ES (a
  suffixed entry point, so `GLCapabilities::clipControlIsEXT` sits beside the
  flag), absent on virgl - where every depth lands in the far half of the range
  and only a read-back can tell.
- **`baseVertex` and `firstInstance` are the attribute pointers', not the
  draw's.** GL puts each behind something the floor has not - base instance is
  4.2 and never ES, base vertex never ES 3.0 - while the whole of what either
  means, no shader in the tree reading a vertex or an instance id, is where a
  slot starts reading. So both are folded into the offsets `applyVertexLayout`
  hands `glVertexAttribPointer`, and every context draws through
  `glDrawArraysInstanced` and `glDrawElementsInstanced` alone. Both are part of
  what the layout cache diffs on.
- **The uniform ring belongs to the `Frame`**, one buffer made on the first
  `setBytes` and deleted with the frame, written at a rising aligned offset and
  respecified from zero when it fills - which no draw can see, GL running its
  commands in order. `setBytes` keeps the bytes rather than writing them, so
  the block is sized at the draw by the pipeline the draw ends up on:
  `glBindBufferRange` must cover the block std140 made, which may be larger
  than what the caller handed over.
- **The VAO and how many of its attribute arrays are enabled live on the
  `GLContext`**, not on the frame or the pass: a VAO belongs to the context
  that made it, and what the last draw enabled outlives every frame.
- **A Vulkan binding number is not a GL one.** A texture is
  `maxTextureSlots + slot`, a render storage buffer `RenderPass::bufferBase +
  slot` and a kernel's uniform block `ComputePass::uniformBase` - and a driver
  need offer only eight shader-storage binding points, which binding 24 is
  past. `glTextureUnitFor`, `glStorageBindingPoint` and
  `glUniformBindingPoint` take the base off, in `RenderPipeline-GL.cpp` beside
  the link-time bind that is their first caller.
- **A pass's clears set the masks and the scissor first**, since every clear is
  subject to both, and the pass records that as its last-applied state so the
  first `setPipeline` diffs against what is really there. The first
  `setPipeline` of a pass applies the whole struct all the same: what the pass
  before it left is not this pass's to assume.
- **The creation path forgets the error queue first.** `glGetError` is what
  decides whether a texture exists, and an error another call left queued would
  otherwise be read as this one's - which is how a `glClientWaitSync` on a
  deleted fence turned into a texture that would not create, two hundred cases
  later. `glForgetErrors()` beside `glDrainErrors`, called once per texture
  creation; nothing else polls the queue outside `EACP_GL_DEBUG`.

**D7 — y is flipped for off-screen targets by wrapping `main`.** eacp's rule
is Metal's: texel row 0 is the top of the picture, uv (0, 0) samples it,
NDC y = +1 is the top of the target. Uploading rows in image order already
gives GL the first two. The third fails only for render targets: GL
rasterizes NDC +1 into the *highest* row, so a target sampled later, or
read back, would come out upside down, while a window (whose row 0 GL also
calls the bottom) comes out right. `ARB_clip_control` fixes it in one call
and virgl on this VM does not offer it. So the lowering pass (D3) renames a
vertex stage's `main` to `eacpMain` and appends

```glsl
uniform float eacpClipYSign;
void main() { eacpMain(); gl_Position.y *= eacpClipYSign; }
```

which is legal because a stage may read its own outputs. A pass on a
texture sets the sign to −1 and flips `glFrontFace` and the y of the
scissor and viewport rects; a pass on the drawable sets +1. Read-back on
a texture that was rendered into then returns memory row 0 first, which is
the top row, so `Texture::read` and `renderNativeContent` need no flip of
their own. This is the same trick SPIRV-Cross and ANGLE use; the wrapper is
the *only* thing the lowering injects.

*As built (stage 1).* The wrapper is the only *code* injected; the lines the
lowering adds beside it are the stage macro and the ES precision declarations
(D3, as built). A vertex lowering renames every `void main()` in the text,
including the fragment half's — that half is behind the macro this source does
not define, so it never reaches the compiler — and a vertex source with no
`void main()` at all is refused.

*As built (stage 2).* The sign is −1 on a texture and +1 on a drawable, as
sketched, and the front face is reversed with it. The scissor and the viewport
are the other way round from the sketch's "flips ... the y of the scissor and
viewport rects": a texture's row 0 *is* its own y = 0, so a rect given in
target pixels needs no flip there and needs one on a drawable, whose row 0 GL
calls the top of the screen. `glClipControl` is taken for the depth half of
clip space (D6 as built) and never for the y half, so the wrapper is the whole
flip on every context.

**D8 — Which backend a `Device` gets.** `EACP_GPU_BACKEND=vulkan|gl|auto`
(default `auto`), read once, in `GPU/Linux/LinuxGPUBackend-Linux.cpp`,
beside the window system's `EACP_WINDOW_SYSTEM` and answering the same
kind of process-wide question. `auto` is: Vulkan when its best physical
device is not `PHYSICAL_DEVICE_TYPE_CPU`; else GL when EGL names a device
that is not software (`EGL_EXT_device_enumeration` with
`EGL_MESA_device_software` absent, so no renderer string is matched by
name); else Vulkan, because llvmpipe has the fuller feature set and is the
test baseline. `EACP_VK_SOFTWARE` keeps its meaning inside the Vulkan
choice. `Device::isValid()` stays the one thing tests read, and
`DevicePresenceTests` prints the backend beside the device name.

*As built (stage 0).* `EACP_GPU_BACKEND` is read once into
`getRequestedGPUBackend()`; `gl` and `composite` log once that no such backend
is built and fall through to Vulkan, and `auto` is Vulkan. The name printed
comes from a new `Device::backendName()`, implemented on all three backends
("Metal", "D3D12", and the `DeviceBackend`'s own name on Linux).

*As built (stage 4).* The rule is `chooseAutoBackend(vulkan, gl)`, a pure
function of two `GPUDeviceClass` answers, with one line the sketch did not
have - no Vulkan at all and a software GL takes the GL - and two probes that
cost an instance and a display and neither of them a `Device`: `vulkanDeviceClass()`
creates a bare `VkInstance`, ranks its physical devices with `selectPhysicalDevice`'s
own filter and destroys it, and `glDeviceClass()` reads the display stage 2's
probe already kept up. Vulkan is asked first and GL is not asked where Vulkan
answered hardware. A copy that *named* a backend keeps it even where no context
came up; only the rule's own GL choice falls back. See stage 4 below.

*As built (stage 2).* `Device::supportsCompute()` is **false on the GL backend
whatever the context has**, not merely below GL 4.3, until stage 6 builds the
tier: `makeGLComputePipeline` answers a backend that is never valid and says so
once in the log, and `beginCompute` on both `Frame` and `CommandBuffer` answers
null, which the portable halves already drop every dispatch under. So llvmpipe's
own compute stage is unused here and the UI takes the mesh route (D10) on it as
it would on a GL 3.3. Stage 6 flips the query to the capability. There is no
`ComputePass-GL.cpp` yet: a null pass needs no object.

**D9 — Compute on GL is a tier, not a fork.** When `computeShaders` is set
(GL 4.3, ES 3.1) the GL backend makes real `ComputePipelineBackend` and
`ComputePassBackend` objects from the same lowered GLSL (`#version 430` /
`310 es`, `std430` buffers, `imageStore`, `atomicAdd`, `shared`,
`glDispatchComputeIndirect`), with `glMemoryBarrier` where the Vulkan
backend hoists its barriers: after every dispatch that precedes a pass, at
`ComputePass::barrier()`, and at the end of every recording. Where it is not
set, `Device::supportsCompute()` — a new query beside
`supportsHalfSimdMatrix` — is false, `beginCompute` asserts, and the UI
takes the mesh route (D10). The two SIMD-matrix queries and the packed
weight paths answer false on every GL version.

**D10 — The UI works without compute.** `ComponentHost` reads
`supportsCompute()` once and, when false, sets every `PathShape`'s backing
to the mesh route `buildMesh` already implements and leaves the coverage
atlas empty but for its opaque texel, so the fragment stage's multiply is
unchanged. Edges come from the target's MSAA rather than analytic coverage.
This lands in stage 0, before any GL code, because it is what makes a
device with no compute a device the UI runs on, and it is testable on
lavapipe by forcing it (`EACP_GPU_NO_COMPUTE=1`, a test-only override read
by the same query).

*As built.* `ComponentHost` asks once, on the first frame that rasterizes a
path rather than in its constructor (asking is what makes the `Device`), and
passes the answer down as a `meshOnly` flag to `PathShape::rasterize` rather
than writing `Backing::Mesh` into every shape: the flag overrides a
`Backing::Mask` the widget asked for, drops the `Automatic` size threshold so
the smallest shape is meshed too, and **drops** a shape the triangulator
cannot read - counted by `wasDropped()`, drawn as nothing - because there is
no mask route left to fall back on. The compute pass is then not begun at all,
which the gathered batch being empty would have achieved on its own; the check
beside it says so rather than leaving it to be rediscovered. The override is
read on every call, like `EACP_NO_PACKED_SIMD_MATRIX` beside it, so a test can
take the tier away and give it back.

**D11 — The composite device: GL render, Vulkan compute.** On a machine
whose GL has no compute and whose Vulkan is a CPU one — this VM — the UI
can still have analytic coverage and every kernel in the tree can still
run. `Composite` is a third `DeviceBackend`, built from a GL backend and a
Vulkan backend on the same thread, and chosen by D8's `auto` only in that
one case (`EACP_GPU_BACKEND=composite` forces it). A resource that crosses —
a `Texture` created `computeWrite`, or a `Buffer` first bound in a compute
pass — has a twin on the other side, made at creation for the texture and
at first bind for the buffer (copying its contents across). The Vulkan
backend's per-recording use tracking already knows what a dispatch wrote,
so `Frame::beginPass()` after a `beginCompute()` in the same frame waits
for the Vulkan work, reads each written twin (a memcpy on llvmpipe: the
memory is host-visible and mapped) and uploads it into the GL object —
dirty rectangles for a texture where the pass reported them, the whole
resource otherwise — before the first draw. The other direction, a pass
rendering into a texture a kernel then reads, is the same copy backwards.
`Device::crossingBytesThisFrame()` and a per-frame count sit beside the
`FrameTimer` so the cost is never hidden, and `setVertexStorageBuffer` on a
GL that has no SSBO is refused rather than copied into nothing.
Timestamps report two clocks, one per side, not a sum.

*As built (stage 5).* The shape is the sketch's; eight things differ, and the
first two are what the sketch would have got wrong.

- **The crossing is pulled at the bind, not pushed at the pass boundary.** A
  crossing resource answers `nativeBuffer()`/`nativeTexture()` for whichever
  side is asking, and squaring that side with the other is what answering does.
  `Frame::beginPass` still finishes the compute recording first - the submit and
  the wait are there - but the copy itself happens inside the first bind that
  wants it, so a pass that binds nothing a kernel wrote pays nothing. Which side
  is asking is one flag on the Device backend, raised for the life of a
  composite compute pass and around `CommandBuffer::fill`, and at rest on the
  render side.
- **What a dispatch wrote is what the composite's own pass saw bound**, not the
  Vulkan per-recording use tracking. `ComputePassBackend::setOutputBuffer` and
  `setOutputTexture` are exactly the writes, and the composite wrapper marks
  them as it forwards - which needs no Vulkan header and holds for a backend
  that tracks nothing. `Frame::beginPass(target, ...)` is the same mark the
  other way round.
- **The only write that reports a rectangle is a host `update`.** A dispatch
  names no region and neither does a render pass, so both owe the other side the
  whole resource; `Texture::update(region, ...)` owes exactly its rectangle and
  `Buffer::update` exactly its bytes. So the dirty-rectangle half of the rule is
  real and tested, and the kernel's own output crosses whole - which for the
  coverage atlas is the atlas.
- **A twin is not seeded unless there is anything to seed it with.** A buffer
  created empty and bound as a kernel's output, or a `computeWrite` texture
  created with null pixels, owes the render side nothing when its twin is made;
  copying undefined bytes would be a crossing charged for nothing.
- **A buffer's twin is device storage rather than the host mapping the sketch
  predicted**, because a `Storage` buffer keeps device storage on the Vulkan
  backend for the reason that backend already states - a kernel writing it is
  what a persistent mapping cannot carry - so the read back is a staged copy and
  not a memcpy. It is still a copy through host memory, which is what the
  counters count.
- **Three counters, not two.** `crossingMillisecondsThisFrame()` joined the
  bytes and the count, timed with a `steady_clock` pair per crossing around the
  copy alone (the submit and wait that precede it are the dispatch's own time).
  It is the number the README leads with.
- **`nativeDevice()` and `nativeQueue()` are the compute half's**, both of them:
  OpenGL has no queue at all and what it would offer as a device is an
  EGLContext - a context per Device over one driver rather than the driver
  itself - so the Vulkan pair is the only answer that means here what it means
  on every other backend. `nativeSampler` does follow the side.
- **The seam needed three additions**, all small: `DeviceBackend::sideFor(GPUApi)`,
  which is what `getVulkanContext` and `getGLContext` read now that one Device
  has one of each; `CrossingResource` with `BufferBackend::crossing()` and
  `TextureBackend::crossing()`, null on every backend but this one; and
  `getBufferBackend`/`getTextureBackend`, friends of `Buffer` and `Texture`, so
  a composite pass can reach the backend behind an object it was handed rather
  than going by a native handle whose type depends on who asked.

Two things the sketch did not have to say and that fell out for free. A
`ShaderLibrary` and a pipeline each live on exactly one side, decided by
`ShaderSource::isCompute()`, so neither needs a wrapper and neither is ever
compiled for the half that would refuse it. And a `Frame` is the render half's
with a Vulkan `CommandBuffer` beside it, opened on the first `beginCompute` -
which is what keeps the two clocks apart without any arithmetic: a labelled
compute pass on a composite `Frame` is timed by that command buffer's own
`CommandTimer` and is therefore **not** in `Device::lastFrameTimings()`, which
is the render half's alone.

The selection order that falls out of D8, D9 and D11: Vulkan on hardware;
GL with GL compute; GL render with Vulkan CPU compute; GL render with the
mesh route.

**D12 — Tests are the same suite, run again.** No GL-specific test file
beyond the lowering unit tests and one capability test. The 79 GPU test
files (33 of them compute) run unchanged on the GL backend; the compute
ones self-skip on `supportsCompute()` exactly as every case self-skips on
`isValid()`. The Vulkan CI lane gains steps that run the suite with
`EACP_GPU_BACKEND=gl` on llvmpipe's GL, headless, then under `with-weston`
and `with-xvfb` for the `Present` cases, then capped to the floor
(`MESA_GL_VERSION_OVERRIDE=3.3`) and as ES (`EACP_GL_ES=1
MESA_GLES_VERSION_OVERRIDE=3.0`). `EACP_REQUIRE_GPU=1` means what it means.
The dev VM is where virgl itself is exercised; nothing on CI has a
hardware GL.

## 3. Files

Renamed or split (pure refactor, every suite stays green — stage 0):

- `GPU/{Buffer,CommandBuffer,Device,Frame,Pipeline,Shader,Texture,Timing,View}/*-Linux.cpp`:
  bodies move to `*-Vulkan.cpp` in the same directory; the `-Linux.cpp`
  that remains is the forwarder (D1).
- `GPU/Vulkan/VulkanContext-Linux.cpp` and the two headers: unchanged but
  for the `DeviceBackend` implementation that constructs a `VulkanContext`.
- `Core/Utils/Pimpl.h`: a constructor taking a ready `std::shared_ptr<T>`.
  *As built: untouched — each Linux `Native` holds the `unique_ptr` itself.*
- `UI/Host/ComponentHost.cpp`, `UI/Render/PathShape.{h,cpp}`: the
  no-compute route (D10).

New:

- `GPU/Linux/GPUBackend-Linux.h`: the twelve abstract backends (D1), and
  `GPU/Vulkan/VulkanBackend-Linux.h` beside it, the Vulkan factory per class.
- `GPU/Linux/LinuxGPUBackend-Linux.{h,cpp}`: `EACP_GPU_BACKEND`, the `auto`
  rule, `makeDeviceBackend()` (D8).
- `GPU/Codegen/GlslLowering.{h,cpp}`: `GlslTarget`, `lowerGlsl` (D3, D7).
  Portable, in `eacp-gpu-codegen`, built and tested on every platform.
- `GPU/Spirv/SpirvCompiler.{h,cpp}`: `validateGlsl`, a lowered source against
  a `GlslTarget` (as built: its own entry point, and the plain GLSL front end
  rather than the OpenGL client — D3 as built).
- `CMake/FindGLBackend.cmake` → `eacp-gl`; `ThirdParty/glad/` with its
  README naming the generator command and the extension list (D5).
- `GPU/OpenGL/GLContext-Linux.{h,cpp}`: the EGL display, the context per
  device, `GLCapabilities` probing, the format table, the thread-local
  current-device switch, the debug-output messenger under
  `EACP_GL_DEBUG=1` (the twin of `EACP_VK_VALIDATION`).
- `GPU/OpenGL/GLCapabilities.h`, `GPU/OpenGL/GLTypes.h`, and
  `GPU/OpenGL/GLBackend-Linux.h` beside them - the GL factory per class, the
  twin of `VulkanBackend-Linux.h`. `GPU/OpenGL/GLUnbuilt-GL.cpp` stood in for
  the render half between the two halves of stage 2 and is gone with it.
- `GPU/{Buffer,...}/*-GL.cpp`: one per class. The resource half and the render
  half both landed in stage 2, `GPUView-GL.cpp` with them as the headless
  backend stage 3 fills in, and `ComputePipeline-GL.cpp` as D9's refusal;
  `ComputePass-GL.cpp` is stage 6's alone.
- `GPU/Linux/CompositeBackend-Linux.{h,cpp}`: D11, stage 5. *As built: 1,449
  lines, and three additions to `GPU/Linux/GPUBackend-Linux.h` beside it -
  `DeviceBackend::sideFor(GPUApi)`, `CrossingResource` with a `crossing()` on
  the buffer and texture backends, and `getBufferBackend`/`getTextureBackend`.*
- `Tests/GPU/GlslLoweringTests.cpp` (portable), `Tests/GPU/GLCapabilityTests-Linux.cpp`,
  `Tests/GPU/BackendSelectionTests-Linux.cpp` (stage 4) and
  `Tests/GPU/CrossingTests.cpp` (stage 5).
- `.github/workflows/build.yml`, `Dockerfile`, `Scripts/with-weston`,
  `Scripts/with-xvfb`: the GL runs (D12).
- `Lib/eacp/GPU/README.md`: an "OpenGL" section beside "Linux";
  `CLAUDE.md` and `README.md`: the backend list and the selection rule.

Unchanged: every public header in `GPU/` but `Device.h`
(`supportsCompute` and `backendName` in stage 0;
`uniformBufferOffsetAlignment` and the crossing counters later),
the emitter, the Metal and D3D12 backends, the window-system code — the GL
backend consumes the same `NativeSurfaceHandle` and `ViewSurface` hooks the
Vulkan one does.

*As built (stage 3): the window-system code gained one function.*
`Graphics::linuxPresentationConnection()` (`Graphics/View/View-Linux.h`, defined
in `Window/LinuxWindowSystem-Linux.cpp` beside `linuxSeat()`) answers the
preferred backend's connection as a surfaceless `NativeSurfaceHandle`, which is
what an EGL display has to be opened on (D2 as built). The `ViewSurface` hooks
and the handle itself are untouched, and `X11Connection` gained a
`getScreenNumber()` accessor for the same call.

## 4. Stages

Each stage is one merge, green on all three Linux lanes and on macOS and
Windows.

**Stage 0 — runtime backend seam.** D1, D8 with only the Vulkan backend
behind it (`EACP_GPU_BACKEND=gl` logs that no such backend is built and
falls through to Vulkan), `supportsCompute()` answering true on every
existing backend, D10 with `EACP_GPU_NO_COMPUTE=1` forcing the mesh route
on lavapipe so `UITests` covers it. No behaviour change otherwise; the
headless suite, `with-weston` and `with-xvfb` must match their baselines
exactly (`GPUTests`, `GPUWidgetsTests`, `UITests` case counts recorded here
when the stage lands). ~900 lines moved, ~500 new.

**Stage 1 — GLSL lowering.** D3 and the D7 wrapper, `GlslLoweringTests`
pinning every rule on both emitted and hand-written sources, the glslang
OpenGL-client validation of every emitted GLSL source and twin at the four
targets. No device is involved; this stage is green on every lane
including the two with no GPU. ~600 lines, ~300 of tests.

**Stage 2 — GL render, headless.** D2, D4, D5, D6: `eacp-gl`,
`GLContext`, and the render-half `-GL.cpp` files, on surfaceless EGL over
llvmpipe. The bar is the render half of `GPUTests` and all of
`GPUWidgetsTests`' and `UITests`' non-compute cases passing pixel-for-pixel
against the same expectations Vulkan meets, through `renderNativeContent`,
with `EACP_GPU_BACKEND=gl EACP_REQUIRE_GPU=1` on the Vulkan lane as a
fourth test step. `Texture::read`, `CommandTimer` on a labelled render
pass, and the `BC*` formats behind their flags. ~3,500 lines, ~200 of tests
(the capability test; everything else is the existing suite).

*Done.* The whole of it is in. The first half is `eacp-gl`, `GLContext`, the
format table, the capability probe and the resource classes; the second is the
render half - `RenderPipeline-GL.cpp` (one linked program per pipeline, every
block and sampler bound by name after linking, the vertex layout and the plain
state struct), `Frame-GL.cpp` (the FBO a frame targets, the load and clear
actions, the per-frame uniform ring), `RenderPass-GL.cpp` (the state diff, the
VAO, the binds, the draws and the MSAA resolve), `CommandBuffer-GL.cpp` (fill,
a fence for submit/wait/isComplete/commitAsync, the `CommandTimer`),
`ComputePipeline-GL.cpp` (D9's refusal) and `GPUView-GL.cpp` (nothing to
present, so the view's content goes through the off-screen
`renderNativeContent` of `GPUView-Linux.cpp`). `OpenGL/GLUnbuilt-GL.cpp` and
`glRenderHalfIsBuilt()` are gone, and `GpuTimestamps` now reports support from
`timerQuery` alone.

Measured, one binary at a time, headless:

| Run | `GPUTests` | `GPUWidgetsTests` | `UITests` | `GPUCodegenTests` |
| --- | --- | --- | --- | --- |
| Vulkan, `EACP_VK_SOFTWARE=1` | 495 | 57 | 184 | 117 |
| GL, llvmpipe core 4.5 | 495 | 57 | 184 | 117 |
| GL, llvmpipe ES 3.2 (`EACP_GL_ES=1`) | 493 + 2 crashed | 57 | 184 | - |
| GL, virgl core 4.0 | 490 + 4 failed | 57 | 184 | - |

All passing except where stated. The line a CI step needs is

```
EACP_HEADLESS=1 EACP_GPU_BACKEND=gl EACP_REQUIRE_GPU=1 LIBGL_ALWAYS_SOFTWARE=1
```

with `EACP_GL_ES=1` added for the ES run; `MESA_GL_VERSION_OVERRIDE=3.3` for the
floor run is stage 4's, along with the step itself (nothing in
`.github/workflows/build.yml` or the `Dockerfile` is touched here).

**The compute half self-skips rather than fails.** `Device::supportsCompute()`
is false on the GL backend whatever the context's own compute stage says (D9 as
built), so the 146 `GPUTests` cases that dispatch, the kernel half of
`GPUWidgetsTests` and the compute branch of `UITests`' `ComponentHost` case all
take the same `return` they take on a device that is not there. The check is
`computeIsAvailable()` - `Tests/GPU/Common.h` for the GPU suite,
`probe::computeIsAvailable()` in `Tests/GPUWidgets/CoverageProbe.h` for the
widget one, where the two dispatching helpers ask it so no case had to. Three
cases say something about the tier rather than using it and were rewritten
instead: `GPU/computeIsSupportedAndCanBeTakenAway` asks the backend's name,
`GLCapability/theDeviceReportsWhatTheContextSaid` now pins the false, and
`ComponentHost/aHostWithoutComputeMeshesEveryPath` compares against the compute
route only where there is one.

**virgl (GL 4.0, no `LIBGL_ALWAYS_SOFTWARE`), four cases**, which is stage 4's
list and none of them a thing this backend does wrong:

- `DepthTexture/aLaterPassReadsTheDepthAnEarlierOneWrote`,
  `MultisampledTarget/oneSampleReadsTheSameDepth` and
  `MultisampledTarget/theDepthPlaneResolvesForSampling` read an absolute depth
  value back. Clip space leaves depth in [0, 1] on the other three backends and
  in [-1, 1] on GL, and `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` is what
  the context takes to say so - core 4.5, `GL_EXT_clip_control` on ES, and
  absent on virgl, which therefore lands every depth in the far half of the
  range. Ordering is untouched, so every depth *test* is right there and only a
  read-back differs.
- `GPU/codegenBufferReadCompiles` needs a storage buffer, which is core 430 and
  ES 310; virgl is 400 and the lowering refuses the source with
  `needsNewerTarget`, exactly as D3 says it should. There is no public query for
  "this device has storage buffers" to self-skip on yet - `Device` reports the
  alignment and nothing else - so this waits for stage 4.
- `LargeBuffer/aBufferPastTwoGigabytesAddressesItsEnd` was left out of the run
  for the reason below.

**llvmpipe's ES 3.2 profile, two crashes**, both a Mesa bug rather than this
backend's: a draw whose *fragment* stage declares a storage block segfaults
inside the driver before any bind - it crashes with the `glBindBufferRange`
removed, and the same program compiles and links without complaint - so
`RenderRanges/storageBufferReadsFromTheOffset` and
`RenderRanges/anUnbindableStorageRangeBindsNothing` take the process down there.
Everything else in all four suites passes on ES.

**Two bugs the other two drivers found in the first half**, both fixed here
rather than recorded: a `BGRA8Unorm` texture was corrected twice on a profile
with no `GL_BGRA` - the upload rows swapped on the CPU *and* a
`GL_TEXTURE_SWIZZLE_R`/`_B` pair - so every sample of one came back with red and
blue exchanged (the whole of `UITests`' layer, image, shadow and clip coverage
on ES). The swizzle is gone and `GLFormat::swappedBGRA` is the CPU swap alone,
which is also what makes a BGRA render target come out right. And
`UI::CoverageAtlas` asked for a `computeWrite` texture unconditionally; a
context with no image store - virgl - refuses one, which took the atlas's opaque
texel with it and left every UI fragment multiplying by nothing (13 of
`UITests`). It now asks for `computeWrite` only where `supportsCompute()` is
true, which is D10's rule one line further down.

**A virgl quirk found on the way**, for stage 4's list:
`LargeBuffer/aBufferPastTwoGigabytesAddressesItsEnd` wedges the guest's
virtio-gpu control queue - the process goes into uninterruptible sleep in
`virtio_gpu_queue_ctrl_sgs` and cannot be killed. It is a `glBufferData` of
over 2 GB, which virgl has to mirror on the host; llvmpipe takes the same call
without complaint. Nothing to fix in this backend, but a full `GPUTests` run on
virgl has to leave that case out.

**Stage 3 — GL present.** `GPUView-GL.cpp`: an `EGLSurface` over a
`wl_egl_window` on the view's subsurface (Wayland) or the view's child
window through `EGL_EXT_platform_xcb` (X11, no Xlib), `eglSwapInterval(0)`
so the main thread never blocks in `eglSwapBuffers` and the existing
frame-callback loop paces as it does for Vulkan, rebuilt on resize, torn
down synchronously on `onLost`. `PresentTests-Linux.cpp` under
`with-weston` and `with-xvfb` on llvmpipe's GL, and `EmbeddedView`'s
`Present/anEmbeddedViewPresentsIntoItsHost` over the same child window.
~700 lines.

*Done.* `GPUView-GL.cpp` is the whole of it as sketched - the `EGLSurface` over
a `wl_egl_window` or an X11 child window, `eglSwapInterval(0)` with the existing
frame-callback loop pacing it, `eglSwapBuffers` as the `Frame` goes, and a
synchronous teardown on `onLost` - plus the drawable half of `Frame-GL.cpp` and
`RenderPass-GL.cpp` under it. Six things the sketch did not say:

- **The EGL display is opened on the window system's platform, from the window
  system's own connection** (D2, D5 as built). A window surface can only be made
  on a display of its own platform, and on Wayland sharing the *connection* is
  not optional either: the buffers the driver attaches to a `wl_surface` have to
  be made on the display that surface belongs to. So `glOpenDisplay()` asks
  `Graphics::linuxPresentationConnection()` first and opens
  `EGL_PLATFORM_WAYLAND_KHR` on that `wl_display`, `EGL_PLATFORM_XCB_EXT` on that
  `xcb_connection_t` and screen, or `EGL_PLATFORM_SURFACELESS_MESA` where the
  copy is headless. One consequence to record for stage 4: **a copy whose
  preferred window system is Wayland cannot present an `EmbeddedView`**, which is
  always X11 whatever a toplevel prefers - the Vulkan backend can, its instance
  carrying both platform extensions at once. Every host that embeds is an X11
  copy (`Platform::isDLL()` prefers X11), so nothing in the tree hits it.
- **A context that starts surfaceless has no draw buffer.** EGL sets the default
  framebuffer's draw and read buffer at the *first* `eglMakeCurrent` and never
  again, and a first bind with `EGL_NO_SURFACE` sets both to `GL_NONE` - which
  every context here does, the `Device` coming up long before any view is shown.
  The first surface a view binds is therefore told `glDrawBuffer(GL_BACK)` /
  `glReadBuffer(GL_BACK)` (`glDrawBuffers` on ES). Without it every frame draws,
  swaps and reports success into an empty window, and **every case in
  `PresentTests` passed anyway**: they count frames and measure the drawable, and
  none of them had ever looked at a pixel. So one case now does -
  `Present/whatIsPresentedIsOnTheWindow` reads a pixel back off the window with
  an xcb connection of its own (X11 only: it is the one window system here whose
  server hands a window's contents back, inferiors included) and checks it is the
  colour the view cleared its frame to. It passes on both backends and fails on
  a GL build with the draw buffer left alone, which is how it was checked.
  `GPUTests` links `eacp-x11` for it.
- **The config is chosen by trying.** Only the native window knows which visual
  it was made with, and an X11 config that does not match it is refused, so
  `GLContext::windowConfigs(depth, stencil)` hands back every single-sampled
  window-capable RGB8 config the display offers - EGL's own sort order puts the
  opaque, planeless ones first - and the view tries them in turn, falling back to
  the plain list where none carried the planes it asked for. Nothing queries a
  visual, which would want xcb in the GPU module.
- **Depth and stencil come from the config; MSAA comes from a companion.** A
  view that asked for depth gets a config with a depth plane (both Mesa
  platforms offer one) and draws straight into the default framebuffer with no
  copy at all. A view that asked for more than one sample - or for a plane no
  config carried - draws into a renderbuffer companion framebuffer that the end
  of the pass blits into the default one, resolving the samples in the same
  call, which is the Vulkan shape and the one the multisampled texture path
  already had. Multisampled *configs* are deliberately skipped: a companion
  resolving into a multisampled default framebuffer is an invalid blit, and
  leaving the surface single-sampled is one rule instead of two.
- **`GLDrawable` is what a presenting view hands a `Frame`** (`GLTypes.h`): the
  framebuffer to draw into, whether it resolves into the default one, the size,
  the sample count, the planes, and a `present` callback the frame's destructor
  calls - so the present happens where every other backend's does and the Frame
  names no EGL type. `GLRenderEncoder` gained `samples` and `resolveToDefault`
  beside it, and `targetSamples()` now reads the encoder rather than the target
  texture, a drawable having none. `setFramesInFlight` is a no-op here: a GL
  context is one stream and what paces a frame is the window system's callback.
- **A resize is one call on Wayland and none on X11.** `wl_egl_window_resize`
  where the client names the buffer size; nothing on X11, where the view surface
  has already configured the window and the driver reads its geometry. The
  drawable's size is the record's either way, which is what the rest of the
  stack lays out against.

Measured, one binary at a time. Headless (`EACP_HEADLESS=1`, `EACP_REQUIRE_GPU=1`):

| Run | `GPUTests` | `GPUWidgetsTests` | `UITests` | `GPUCodegenTests` |
| --- | --- | --- | --- | --- |
| Vulkan, `EACP_VK_SOFTWARE=1` | 496 | 57 | 184 | 117 |
| GL, llvmpipe core 4.5 | 496 | 57 | 184 | 117 |

`GPUTests` is 495 + `Present/whatIsPresentedIsOnTheWindow`, which self-skips
with no display. With a display server, `EACP_REQUIRE_DISPLAY=1` and
`EACP_REQUIRE_GPU=1`:

| Run | `Present/` | `EmbeddedView/` | `Wayland/` | `X11/` |
| --- | --- | --- | --- | --- |
| `with-weston`, Vulkan | 11 | - | 13 | - |
| `with-weston`, GL | 11 | - | 13 | - |
| `with-weston`, GL ES (`EACP_GL_ES=1`) | 11 | - | - | - |
| `with-xvfb`, Vulkan | 11 | 17 | - | 47 |
| `with-xvfb`, GL | 11 | 17 | - | 47 |
| `with-xvfb`, GL ES | 11 | 17 | - | - |

All passing. The whole suite was run the way the CI lanes run it as well -
`ctest -j4` under `with-weston` less the `X11/` and `EmbeddedView/` cases, then
the `X11/`, `EmbeddedView/` and `Present/` ones under `with-xvfb` - and it is
**1814 and 75, all passing, on Vulkan and on GL alike**.

What the suite still cannot see is the picture, so the presented
frames were compared against the Vulkan ones by hand as well, out of tree: the
`Triangle` demo (X11 and, through a nested `weston --backend=x11`, Wayland) and
the same demo forced to 4 samples with depth came back **pixel-identical** to
Vulkan's over the whole window, orientation included, and `Teapot`'s depth pass
matches too. The one-pixel test case is what CI has of that.

**One thing for stage 4's list, and not this backend's.** On the dev VM's own
live desktop session - a real compositor, with the test's window behind the
terminal that started it - three of the `Present/` cases fail on **both**
backends: `continuousModeKeepsPresenting`, `maxFpsPacesContinuousMode` and
`hidingStopsFramesAndShowingResumes`, all three of which need frame callbacks to
keep arriving, which a compositor is free to stop for a window nobody can see.
Checked against the tree before this stage: it fails there too, so it is the
session rather than anything here. The headless lanes, whose compositor always
repaints, are the ones the suite is written for.

**Weston headless and Xvfb with EGL** (the risk §5 names) **both work.** Under
Weston's headless backend Mesa's Wayland EGL falls back to `wl_shm` buffers and
under Xvfb its xcb platform to `XPutImage`, and each carries the whole present
suite on llvmpipe, on both profiles. Two notes for stage 4: Mesa 26.0.8 is what
this was run on, and `weston-screenshooter` cannot capture the headless backend
(it asserts on a zero width), which is why the Wayland picture check went
through a nested X11 Weston instead.

**Stage 4 — selection and the dev VM.** D8's `auto` rule for real, the
capped and ES runs of D12 on CI, the `Dockerfile` and `README` updates,
and the first run of `Apps/GPU`, `Apps/UI` and `Apps/Plugins` on virgl on
the VM, with the driver quirks that turns up recorded in `README.md`'s GL
section the way the Parallels D3D12 quirks are. ~300 lines, mostly CI and
docs.

*Done.* Five things, and one of them was not in the sketch.

**D8's rule as built.** `chooseAutoBackend(GPUDeviceClass vulkan, GPUDeviceClass
gl)` in `LinuxGPUBackend-Linux.cpp` is the rule and nothing else - a pure
function of two answers, which is what lets `Tests/GPU/BackendSelectionTests-Linux.cpp` pin all nine pairs on a machine with neither kind of device. It is
the sketch's three lines plus one the sketch did not have: **no Vulkan at all
and a software GL takes the GL**, since a software device is still a device and
the "else Vulkan" was written against llvmpipe being there. The probes beside it
are `vulkanDeviceClass()` (`VulkanContext-Linux.cpp`: a bare `VkInstance`
created, its physical devices ranked with `selectPhysicalDevice`'s own filter,
and destroyed again - so a copy that goes on to take GL has not built a
`VkDevice` it would never use, which `VulkanShared` would have, `Spirv::warmUp`
and all) and `glDeviceClass()` (`Device-GL.cpp`, over the `glDisplayIsAvailable`
/ `glDisplayIsSoftware` pair stage 2 already had). Vulkan is asked first and GL
is not asked at all where Vulkan answered hardware, so the ordinary machine
never opens an EGL display. Two more things worth having in writing:

- **A copy that names a backend keeps it.** Where the *rule* picked GL and no
  context came up, the Vulkan it was weighed against is taken instead; where
  `EACP_GPU_BACKEND=gl` was passed, it is not - otherwise a CI step pinned to GL
  could quietly become a second run on Vulkan.
- **The Wayland/`EmbeddedView` caveat is documented and not accounted for.** A
  Wayland copy's EGL display cannot carry an X11 `EmbeddedView`, but every copy
  that embeds is a plugin copy and every plugin copy prefers X11
  (`Platform::isDLL()`), so the combination is unreachable in the tree. Forcing
  `EACP_WINDOW_SYSTEM=wayland` on an embedding plugin is the way to reach it and
  `EACP_GPU_BACKEND=vulkan` is the answer there.

On this VM `auto` logs `GPU: auto chose OpenGL (Vulkan: software, OpenGL:
hardware)`; on a runner, with no `/dev/dri` at all, the only EGL device Mesa
enumerates is the one carrying `EGL_MESA_device_software`, so it chooses Vulkan.
The CI lanes do not rely on that: the Vulkan lane now names `vulkan` in its
environment and each GL step names `gl`, so a runner image that one day carries
a GPU cannot flip a lane silently.

**The stage-2 leftovers, as two queries on `Device` rather than one.**
`supportsStorageBuffers()` and `supportsZeroToOneDepth()`, true on Metal, D3D12
and Vulkan, and each false on exactly the GL contexts that made a case fail:
`GPU/codegenBufferReadCompiles` and the two `RenderRanges/` storage cases skip
on the first, the three depth read-back cases on the second, through
`storageBuffersAreAvailable()` / `zeroToOneDepthIsAvailable()` in
`Tests/GPU/Common.h` beside `computeIsAvailable()`. **virgl is then clean**: 501
of 501, with every skip on it explained by a capability something can ask for.

One fix underneath them, found by the floor run rather than by virgl:
`GLCapabilities`' `computeShaders`, `storageBuffers` and `imageLoadStore` are
now taken from `glslTarget()` rather than from the extension string, because a
kernel, a std430 block and an image store are things the *language* has and the
lowering emits no `#extension` line. A driver capped with
`MESA_GL_VERSION_OVERRIDE=3.3` still advertises `ARB_compute_shader`, so without
this the floor run reported a compute stage it could not be handed a source for
and `GLCapability/computeImpliesStorageBuffersAndImageStore` failed on it.

**The CI steps.** Five on the Vulkan lane, after its three: the three device
binaries headless on `EACP_GPU_BACKEND=gl LIBGL_ALWAYS_SOFTWARE=1`, the
`Present/` cases again under `with-weston` (`-R '^(Wayland|Present)/'`) and
under `with-xvfb` (`-R '^(X11|EmbeddedView|Present)/'`), then the same three
binaries capped to the floor (`MESA_GL_VERSION_OVERRIDE=3.3
MESA_GLSL_VERSION_OVERRIDE=330`) and to ES (`EACP_GL_ES=1
MESA_GLES_VERSION_OVERRIDE=3.0`). `libegl1 libegl-mesa0 libgles2
libgl1-mesa-dri` joins the *runtime* apt line - the one the Vulkan ICD is on,
not the shared build one, because the other two Linux lanes are deliberately
device-free and a GL driver on them would turn their self-skipping GPU suites
into a third full run. The `Dockerfile` has the same four packages and the GL
commands in its documented steps; it is **untested**, this machine having no
docker.

**The two Mesa ES crashes need no exclusion list.** The ES step is pinned to
ES 3.0, where the context has no storage buffers at all, so
`RenderRanges/storageBufferReadsFromTheOffset` and
`anUnbindableStorageRangeBindsNothing` self-skip on
`Device::supportsStorageBuffers()` before they can reach the driver. The crash
is still there on an uncapped ES 3.2 run - checked, both cases, still a
segfault - and is recorded in the README as Mesa's.

Measured, one binary at a time, headless (`EACP_HEADLESS=1 EACP_REQUIRE_GPU=1`):

| Run | `GPUTests` | `GPUWidgetsTests` | `UITests` | `GPUCodegenTests` |
| --- | --- | --- | --- | --- |
| Vulkan, `EACP_VK_SOFTWARE=1` | 502 | 57 | 184 | 117 |
| GL, llvmpipe core 4.5 | 502 | 57 | 184 | 117 |
| GL, llvmpipe core 3.3 (the floor) | 502 | 57 | 184 | 117 |
| GL, llvmpipe ES 3.0 | 502 | 57 | 184 | 117 |
| GL, virgl core 4.0, less `LargeBuffer` | 501 of 501 | 57 | 184 | 117 |

All passing. With a display: 1820 under `with-weston` less the `X11/` and
`EmbeddedView/` cases and 75 under `with-xvfb`, on Vulkan and on GL alike, and
24 for the `-R '^(Wayland|Present)/'` the new Weston step runs.

**The VM, and what its driver does.** Every `Apps/GPU`, `Apps/UI` and
`Apps/Plugins` executable that builds on Linux - 29 of them - was run on virgl
on both window systems with `EACP_GL_DEBUG=1`, and every one comes up and draws.
`Present/whatIsPresentedIsOnTheWindow` passes against the live XWayland on
virgl, so the pixels are checked and not the frame count alone. The quirks are
in `Lib/eacp/GPU/README.md`'s new "virgl" subsection - no `glClipControl`, no
storage buffers, no compute or image store, no `ARB_buffer_storage`, the >2 GB
`glBufferData` that wedges the guest, timer queries that answer while
`lastFrameTimings()` reads zero, and a debug channel whose only messages are
`GL_DEBUG_TYPE_PERFORMANCE` hints about a `GL_STATIC_DRAW` buffer being
rewritten.

**The measurement §5 asked for: the uniform ring does not show up; the stream
does.** `Apps/GPU/StreamingStress` is 2000 meshes a frame, each its own vertex
write, index write, uniform-ring `glBufferSubData`, bind and `drawIndexed`. Over
the VM's XWayland: 3.0-4.7 ms of CPU a frame on GL/virgl, 3.2-5.8 on
GL/llvmpipe, 2.0-4.0 on Vulkan/llvmpipe - the same order, so writing two
thousand draws costs no more here. What differs is the frame rate: ~13 fps on
virgl against ~57 on either llvmpipe, while an *empty* frame runs at the pacer's
47-50 fps on all three. So the cost is the command stream crossing virtio after
our own measurement has ended, invisible to the CPU timer and to the GPU one,
and the fallback §5 proposed - fewer, larger writes - is not the one that would
help; fewer draws is. Beside it, one non-repeating ~150 ms frame on virgl where
the streaming arenas fold back and four new buffers are created, a
`glBufferData` of a few megabytes being a host-side allocation there.

**One thing confirmed and not fixed**, the stage-3 note: on the VM's own live
GNOME Wayland session a window nobody is looking at stops receiving frame
callbacks, so a continuously presenting view stops - **on Vulkan exactly as on
GL**, checked with a clear-only view on both. The X11 backend, whose pacer is a
timer, runs at 47-50 fps in the same session, which is what every measurement
above was taken on.

**Stage 5 — composite device.** D11, on the VM (the only place it selects
itself) and on CI forced with `EACP_GPU_BACKEND=composite` over two
llvmpipes, which exercises every crossing path with no hardware. The bar
is the whole of `UITests` with analytic coverage, `Apps/GPU/PathCoverage`
and `PathBench` running, and the crossing counters reporting what the
README says they cost. ~1,200 lines, ~200 of tests.

*Done.* Six things.

**The rule's third fact.** `chooseAutoBackend` is now
`(GPUDeviceClass vulkan, GPUDeviceClass gl, bool glHasCompute)`, and the pair
that reaches the composite is a hardware GL with no kernels beside a Vulkan of
any kind. The third answer is `glBackendHasCompute()` in `Device-GL.cpp`, beside
`supportsCompute()` and false for the same reason - D9's tier is stage 6's - and
it is a question about the backend rather than about a context, since the rule
is asked before any `Device` exists. So this VM gets the composite and a runner
still gets Vulkan (two software stacks resolve to the baseline one, kernels or
none: there is nothing to gain by crossing between two llvmpipes). With no
Vulkan at all there is nothing to compose with and the hardware GL is taken
alone. `Tests/GPU/BackendSelectionTests-Linux.cpp` pins the lot, including the
pairs this machine cannot produce.

**What crosses, and how a written twin is found.** The render half is the
primary - a `Buffer` and a `Texture` are created there, and a `computeWrite`
texture is created there *without* the flag, an image store being exactly what
the OpenGL this was built for has not got. The compute half gets a twin at
creation for a `computeWrite` texture and at first kernel bind for everything
else. After that each side records what it owes the other and the copy happens
at the other side's next bind; see the eight *as built* points under D11 for
where that differs from the sketch.

**Three counters on `Device`**, portable and zero everywhere else:
`crossingBytesThisFrame()`, `crossingsThisFrame()` and
`crossingMillisecondsThisFrame()`, cleared in `Device::beginFrame()` beside the
frame timer. `Apps/UI/WidgetGallery`, `Apps/UI/AtlasCeiling`,
`Apps/GPU/PathCoverage` and `Apps/GPU/PathBench` print them, and print nothing
at all on a device with one backend.

**The measurement §5 asked for, and it is the first row that matters.** On the
VM, virgl rendering and llvmpipe dispatching:

| Frame | Crossed | Copies | Wall clock |
| --- | --- | --- | --- |
| `WidgetGallery`, a frame that rasterizes | 270 KB | 7 | 0.41 ms |
| `PathCoverage`, its two star masks | 3.1 MB | 12 | 1.37 ms |
| `AtlasCeiling`, the first frame (a full 4096² atlas) | 64 MB | 7 | 32.9 ms |
| `PathBench`, the whole headless run | 121 MB | 34,960 | 51 ms |

An ordinary interface frame is the first row: under half a millisecond, which
is worth paying for analytic coverage on a machine that would otherwise mesh
every path. The third is the demo whose whole point is to fill the atlas to its
ceiling, and the fourth is thousands of small buffer crossings across a
thirty-second benchmark - a host write lands on the render side and is carried
over at the next dispatch, which is where the count comes from.

**What a composite skips, and on what.** Nothing on `supportsCompute()`, which
is the point of it. On virgl it skips exactly what the OpenGL backend skips and
through the same queries: the three depth read-back cases on
`supportsZeroToOneDepth()`, and four storage-buffer cases on
`supportsStorageBuffers()` - `GPU/codegenBufferReadCompiles`, the two
`RenderRanges/` ones, and `FrameCompute/indexedBufferReadFeedsTheDraw`, which
gained that second guard here: it needs a compute tier *and* a std430 block in
the fragment stage, which is the one combination only a composite can have half
of. On llvmpipe's GL none of the four skip. The one case excluded rather than
skipped is still `LargeBuffer/aBufferPastTwoGigabytesAddressesItsEnd`, which
wedges the guest through virgl whichever backend drives it.

**Apps.** All 27 `Apps/GPU` and `Apps/UI` executables were run on the composite
on the VM under `EACP_WINDOW_SYSTEM=x11` and every one comes up and draws;
`PathCoverage`, `WidgetGallery` and `AtlasCeiling` were run again with
`EACP_GL_DEBUG=1` and the GL debug channel is silent - no errors and not even
the `GL_STATIC_DRAW` performance hint the pure-GL runs draw.
`EACP_VK_VALIDATION=1` could not be exercised: the layer is not installed on
this machine and the run says so and continues.

**Left for stage 6, or for whoever wants the crossing cheaper.** A host write to
a resource whose twin already exists could be written through to both sides
instead of leaving the twin owing a read-back - the bytes are already in host
memory, and it is the read-back that is expensive on a command-stream driver.
And a dispatch that could *name* the rectangle it wrote would turn the coverage
atlas's per-frame crossing from the whole atlas into its dirty part; the
machinery for a rectangle is there and only the report is missing.

**Stage 6 — GL compute tier.** D9 on llvmpipe's GL 4.5 / ES 3.2, which is
where CI can run it; the compute half of `GPUTests` minus the SIMD-matrix
and packed-weight cases. Optional: this VM never reaches it, and the
composite covers the machines that do not either. ~1,500 lines.

## 5. Risks and open questions

- **virgl is a command stream over virtio.** Every state change and every
  `glGet*` is a round trip to the host; D6's diffing and the absence of
  `glGetError` polling outside `EACP_GL_DEBUG` are what keep a UI frame
  cheap, and the first real measurement is stage 4's. If the uniform ring's
  `glBufferSubData` per draw shows up, `ARB_buffer_storage` is absent on
  this VM (probed), so the fallback is fewer, larger writes.
- **Timer queries on virgl** may be disjoint or absent; `GpuTimestamps`
  answers "no timings" then, which `FrameTimer` already tolerates.
- **The `main` wrapper (D7)** assumes a vertex stage spells its entry
  `void main()` with that whitespace, which the emitter and every twin do; a
  hand-written source that does not fails at pipeline creation with the
  lowering's own message. `eacpClipYSign` is a plain uniform set by name
  after linking, which D3's link-time binding already does for blocks.
- **ES 3.0 has no `GL_BGRA` internal format**, and the swizzle-plus-swap
  route costs a CPU pass over every `BGRA8Unorm` upload. `PixelFormat::
  BGRA8Unorm` is the default swapchain and pipeline format on every backend,
  so on ES the swapchain is RGBA and the pipeline's declared format is
  mapped, not the app's. To be measured on ES before deciding whether the
  default should flip there.
- **Weston headless and Xvfb with EGL.** Under Weston's headless backend
  llvmpipe's Wayland EGL platform falls back to `wl_shm` buffers, and under
  Xvfb the xcb platform to `XPutImage`; both are how `glxgears` runs there
  today, but neither is tested in this tree yet and stage 3 finds out.
  *Answered (stage 3): both carry the whole present suite, on core and on ES.*
- **glslang's OpenGL-client validation** (`EShClientOpenGL`) is less
  exercised than its Vulkan one, and the ES profile validation is a
  different code path again. If it proves too loose to catch a leaked
  Vulkan construct, the lowering tests pin the rules textually and the
  device run is the backstop.
- **`MESA_*_OVERRIDE` under llvmpipe** lowers what the driver reports; it
  does not remove a fast path the driver takes anyway. A floor run on CI is
  therefore a *compile and bind* check of the floor path, and the real
  GL 4.0 driver is the VM.
- **Two Devices on one thread** cost an `eglMakeCurrent` per switch. Only
  the tests do it; if it shows, one shared context group per thread is the
  fallback and D2 changes in one file. *Answered (stage 5): a composite is two
  Devices in one and costs no switch at all - the Vulkan half touches no
  current context, so the EGL context simply stays current between dispatches.*
- **The composite doubles crossing resources** and serialises compute and
  render on the CPU. It is for interfaces, whose crossing is one R8 atlas's
  dirty rectangles; a demo that crosses a full-screen texture every frame
  is slow there and the counters say so. Not a reason to build it
  differently, but a reason its README section leads with the cost.
  *Answered (stage 5): an ordinary interface frame crosses 270 KB in 7 copies
  for 0.41 ms and a full 4096² atlas costs 64 MB and 33 ms, so the shape of the
  worry was right and its size is a frame's worth of a demo built to reach the
  ceiling. The atlas crosses whole rather than by dirty rectangle, because a
  dispatch reports no region - the one write that does is a host `update`.*
- **Android** wants the Vulkan backend with a lower floor than 1.3 and an
  `ANativeWindow` surface kind; nothing here should make that harder, and
  D1's seam is the same one an Android build would use to leave GL out.
