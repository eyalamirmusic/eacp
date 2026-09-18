# OpenGL backend — plan

Written 2026-09-17 against `80b93a9a` (branch `opengl-backend`), from a read
of the Vulkan backend, the shader emitter, the GPU test harness, the UI's
coverage atlas and a probe of the development VM's drivers. Line counts are
estimates, not commitments.

## Progress

| Stage | State | Notes |
| --- | --- | --- |
| 0 — runtime backend seam | pending | |
| 1 — GLSL lowering | pending | |
| 2 — GL render, headless | pending | |
| 3 — GL present | pending | |
| 4 — selection and the dev VM | pending | |
| 5 — composite device | pending | |
| 6 — GL compute tier | pending | |

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
- `UI/Host/ComponentHost.cpp`, `UI/Render/PathShape.{h,cpp}`: the
  no-compute route (D10).

New:

- `GPU/Linux/GPUBackend-Linux.h`: the twelve abstract backends (D1).
- `GPU/Linux/LinuxGPUBackend-Linux.{h,cpp}`: `EACP_GPU_BACKEND`, the `auto`
  rule, `makeDeviceBackend()` (D8).
- `GPU/Codegen/GlslLowering.{h,cpp}`: `GlslTarget`, `lowerGlsl` (D3, D7).
  Portable, in `eacp-gpu-codegen`, built and tested on every platform.
- `GPU/Spirv/SpirvCompiler.{h,cpp}`: `compileGlsl` gains a `GlslTarget`
  overload that validates for the OpenGL client / ES profile.
- `CMake/FindGLBackend.cmake` → `eacp-gl`; `ThirdParty/glad/` with its
  README naming the generator command and the extension list (D5).
- `GPU/OpenGL/GLContext-Linux.{h,cpp}`: the EGL display, the context per
  device, `GLCapabilities` probing, the format table, the thread-local
  current-device switch, the debug-output messenger under
  `EACP_GL_DEBUG=1` (the twin of `EACP_VK_VALIDATION`).
- `GPU/OpenGL/GLCapabilities.h`, `GPU/OpenGL/GLTypes.h`.
- `GPU/{Buffer,...}/*-GL.cpp`: one per class, the render half in stage 2,
  `GPUView-GL.cpp` in stage 3, `ComputePipeline-GL.cpp` and
  `ComputePass-GL.cpp` in stage 6.
- `GPU/Linux/CompositeBackend-Linux.{h,cpp}`: D11, stage 5.
- `Tests/GPU/GlslLoweringTests.cpp` (portable), `Tests/GPU/GLCapabilityTests-Linux.cpp`.
- `.github/workflows/build.yml`, `Dockerfile`, `Scripts/with-weston`,
  `Scripts/with-xvfb`: the GL runs (D12).
- `Lib/eacp/GPU/README.md`: an "OpenGL" section beside "Linux";
  `CLAUDE.md` and `README.md`: the backend list and the selection rule.

Unchanged: every public header in `GPU/` but `Device.h`
(`supportsCompute`, `uniformBufferOffsetAlignment`, the crossing counters),
the emitter, the Metal and D3D12 backends, the window-system code — the GL
backend consumes the same `NativeSurfaceHandle` and `ViewSurface` hooks the
Vulkan one does.

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

**Stage 3 — GL present.** `GPUView-GL.cpp`: an `EGLSurface` over a
`wl_egl_window` on the view's subsurface (Wayland) or the view's child
window through `EGL_EXT_platform_xcb` (X11, no Xlib), `eglSwapInterval(0)`
so the main thread never blocks in `eglSwapBuffers` and the existing
frame-callback loop paces as it does for Vulkan, rebuilt on resize, torn
down synchronously on `onLost`. `PresentTests-Linux.cpp` under
`with-weston` and `with-xvfb` on llvmpipe's GL, and `EmbeddedView`'s
`Present/anEmbeddedViewPresentsIntoItsHost` over the same child window.
~700 lines.

**Stage 4 — selection and the dev VM.** D8's `auto` rule for real, the
capped and ES runs of D12 on CI, the `Dockerfile` and `README` updates,
and the first run of `Apps/GPU`, `Apps/UI` and `Apps/Plugins` on virgl on
the VM, with the driver quirks that turns up recorded in `README.md`'s GL
section the way the Parallels D3D12 quirks are. ~300 lines, mostly CI and
docs.

**Stage 5 — composite device.** D11, on the VM (the only place it selects
itself) and on CI forced with `EACP_GPU_BACKEND=composite` over two
llvmpipes, which exercises every crossing path with no hardware. The bar
is the whole of `UITests` with analytic coverage, `Apps/GPU/PathCoverage`
and `PathBench` running, and the crossing counters reporting what the
README says they cost. ~1,200 lines, ~200 of tests.

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
  fallback and D2 changes in one file.
- **The composite doubles crossing resources** and serialises compute and
  render on the CPU. It is for interfaces, whose crossing is one R8 atlas's
  dirty rectangles; a demo that crosses a full-screen texture every frame
  is slow there and the counters say so. Not a reason to build it
  differently, but a reason its README section leads with the cost.
- **Android** wants the Vulkan backend with a lower floor than 1.3 and an
  `ANativeWindow` surface kind; nothing here should make that harder, and
  D1's seam is the same one an Android build would use to leave GL out.
