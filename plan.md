# Linux graphics support — investigation and plan

Written 2026-09-05 against `b0de675`, from five read-only investigations of the
tree (GPU backend contract, shader codegen, Graphics layer and event loop,
dependent modules/tests/CI, external Vulkan-on-Linux research). No code has been
written yet. Line counts are estimates from reading the Metal and D3D12 backends,
not commitments.

## 1. Headline findings

1. **"Linux GPU" is two projects, and Graphics is the critical path.** `eacp-gpu`
   links `eacp-graphics` PUBLIC (`Lib/eacp/GPU/CMakeLists.txt:3`),
   `GPU/Common.h:3` includes the entire `Graphics.h` umbrella (Window, Menu,
   TrayIcon, TextInput, LayerViews), and `GPUView` derives from `Graphics::View`
   (`GPUView.h:19`). Graphics has exactly one Linux file today
   (`HotKey/GlobalHotKey-Linux.cpp`, a 25-line stub) and is never built on Linux
   because of the gate at `Lib/eacp/CMakeLists.txt:5`.
2. **The GPU module itself is exceptionally well factored for a third backend.**
   Zero platform `#if`s across Graphics/GPU/GPUWidgets/Text/Sprites/UI/SVG;
   platform choice is purely CMake file selection against `Pimpl<Native>`. The
   contract is exactly 13 `struct X::Native` bodies + a 13-line
   `nativeShaderSource`. `MipChain`, `StreamingBuffers`, `FrameTimer`,
   `RenderPass.cpp`, `Device.cpp` and all of `Codegen/` (8,899 lines) are already
   portable.
3. **The D3D12 backend is the template, not the Metal one.** It resolves the
   `void*` handles into composite structs via a shared internal header
   (`Windows/D3D12Types.h`) and splits process-wide (`D3D12Shared`) from
   per-device (`D3D12Context`) state. A Vulkan backend needs the same shape:
   `Vulkan/VulkanTypes.h` + `VulkanContext.h`. Estimated 7,000–10,000 lines
   (D3D12 is ~8,300 including the context).
4. **The shader EDSL gets a third text dialect, not a new compiler.**
   `ShaderEmitter.cpp` is one walker with a two-member `Backend` enum and 27
   branch points; 76% of its 1,766 lines are dialect-neutral. A GLSL 450 arm
   costs ~400–450 emitter lines and runs and unit-tests on macOS and Windows CI
   before any Vulkan device exists.
5. **Almost every test is already headless.** No test in GPU, Text, UI, SVG or
   GPUWidgets creates a window; `Device()` has no surface coupling; the Windows
   lane already runs all 304 GPU tests on WARP. Mesa lavapipe is the exact
   analogue, and the offscreen path means the Linux GPU lane needs no display
   server at all.
6. **The event loop already fits.** `EventLoop-Linux.cpp:95-96` is a `poll()`
   over one self-pipe fd. A Wayland or xcb connection fd joins that set in ~20
   lines. SDL/GLFW/GTK all want to own the pump and would break
   `runEventLoopFor` nesting (`EventLoop.h:62-82`).

## 2. Recommendations

| Decision | Recommendation | Why |
|---|---|---|
| Backend API | **Vulkan 1.3 core** as the floor; use 1.4 features opportunistically | Dynamic rendering, synchronization2, extended dynamic state (cull/front-face) all core in 1.3. Every Mesa driver (RADV, ANV, NVK, lavapipe) and NVIDIA ≥510 ship it; DXVK 3.x already requires 1.4 as a gaming baseline. OpenGL maps badly (no command buffers, llvmpipe caps at 4.5). WebGPU cannot express `sampleCount` other than 1/4 nor resolve depth. |
| Shader route | **Emit GLSL 450 from the existing walker (`emitGlsl`), compile at runtime with glslang**, CPM-fetched with `ENABLE_OPT=OFF` | Keeps the "one graph, cannot drift" property; bindings written from the same `RenderPass::uniformBase`/`bufferBase` constants the C++ binder reads. Measured cost is small (§3.3). DXC-over-HLSL was considered: zero emitter work, but a 515 MB Linux tarball with no apt package, a `-fvk-*-shift` table as a second source of truth for bindings, and FXC-era HLSL under a stricter compiler. Direct SPIR-V (3–5k lines) is a later optimisation once GLSL is the reference. |
| Render passes | **`VK_KHR_dynamic_rendering` as the sole path** | Maps 1:1 onto `beginPass`/`~RenderPass` and `MTLRenderPassDescriptor`: per-attachment load/store (= `DepthAction`), `resolveImageView` (= resolve-every-pass). Dawn disables it on Intel ≤ Gen9 / Mali-G68 / PowerVR — see open decision D3. |
| Descriptors | **Pooled descriptor sets**: one growable pool per frame-in-flight, reset when the frame's fence signals; uniform block and `set*Bytes` via `UNIFORM_BUFFER_DYNAMIC` with per-bind offsets; samplers as `pImmutableSamplers` | 7 of 8 comparable frameworks do this. `VK_EXT_descriptor_buffer` is unsupported on lavapipe (kills CI) and is already being superseded by `VK_EXT_descriptor_heap` (ANV default in Mesa 26.2). Push constants cap at 128 bytes, too small for a `Float4x4`-heavy uniform block. |
| Memory | **VMA** (header-only, CPM `v3.4.0`) | D3D12Context uses committed resources everywhere; Vulkan's `maxMemoryAllocationCount` (commonly 4096) makes that model unsafe. `ConstantPage` ring, staging/readback pools keep their shape on VMA. |
| Loader | **CPM Vulkan-Headers + volk**, `dlopen("libvulkan.so.1")` at runtime | The Linux build then needs no new apt package for the GPU half; only Mesa's loader at runtime. `Device::isValid()` false when absent, as today. |
| Window system | **Wayland directly** (libwayland-client + xdg-shell + libxkbcommon + libdecor), structured so an xcb backend can be added later behind the same `View::Native`/`Window::Native` seam | Consistent with "wrap the compositor, don't bundle one". Wayland gives a pollable fd, vsync frame callbacks, fractional scale, VK_KHR_wayland_surface. Fedora 43, Ubuntu 25.10+, GNOME 50, Plasma 6.8 are Wayland-only; XWayland clients get blurry fractional scaling. GNOME refuses server-side decorations, hence libdecor. |
| 2D `Context` | **Skip for the first slice** | `UI`, `Sprites`, `GPUWidgets` are portable C++ over GPU + Text (`UI/CMakeLists.txt:4-7`). The whole GPU-drawn UI tier reaches Linux with one platform file (`GlyphRasterizer-Linux.cpp`) and no Cairo. |
| Text | **FreeType + HarfBuzz + fontconfig** behind the existing `GlyphSource` seam (`Text/GlyphRasterizer.h:84-104`, four virtuals) | Covers ~80% directly; the remaining work is script itemization + per-run fallback re-shaping and porting the CSS weight-matching. |
| CI | Pinned `ubuntu-24.04` lane with `mesa-vulkan-drivers`, `VK_DRIVER_FILES` pointing at lavapipe, **no display server** for GPU/Text/UI tests; `xvfb-run` or `weston --backend=headless` only for the WSI subset later | GH `ubuntu-latest` is still 24.04 (Mesa 25.2 via updates). Lavapipe: Vulkan 1.3 conformant, BC1–7, 4× MSAA, timestamps, dynamic rendering, push descriptors. |
| Deferred | `Camera`, `Video`, native `WebView` | All have honest-stub shapes already (`Camera.h:135-190`, `Decoder.h:41-70`); WebKitGTK would drag in GTK and fight our own Wayland windowing. `eacp-webview-bridge` is already portable and can leave the graphics gate today. |

## 3. What has to be built

### 3.1 The Vulkan backend (`Lib/eacp/GPU/**/*-Linux.cpp` + `Vulkan/`)

| Type | Effort | Notes |
|---|---|---|
| `VulkanShared` / `VulkanContext` / `VulkanTypes.h` | Hard (~3,000 lines, the `D3D12Context` analogue) | Instance, physical device, `VkDevice`, queue + timeline semaphore, command-pool ring, `openRecording` (a buffer upload during a frame lands on the frame's command buffer, `Buffer-Windows.cpp:176-189`), upload arena, constant ring, staging/readback pools, deferred release stamped late (`D3D12Context-Windows.cpp:1076-1135` records two real bugs), descriptor pools, `generation` counter, `DriverQuirks` probe. |
| `Texture` | Hard (the biggest file on both backends) | Formats, cube-as-array, supplied mip chains, BC1/2/3/7 (`textureCompressionBC` must be a real query — on D3D12 `supportsBlockCompression()` is literally `isValid()`, `Device-Windows.cpp:108-111`), MSAA companion with `storeOp=STORE` **and** resolve, depth companion (one `D32_SFLOAT`/`D32_SFLOAT_S8_UINT` image + aspect masks replace four `depth*Format` helpers), sampleable depth via `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` (matches Metal exactly; the `resolveDepthWithShader` fallback is unnecessary), region update/read with the deliberate no-clamp rules, layout tracking. `wrapPixelBuffer` may return invalid on day one as Windows does (`Texture-Windows.cpp:227-230`). |
| `Frame` | Hard | `vkCmdBeginRendering` per `beginPass`, `DepthAction` → `LOAD_OP_CLEAR/STORE_OP_DONT_CARE` · `CLEAR/STORE` · `LOAD/STORE`, full-target viewport + scissor set explicitly at pass begin, stencil reference reset to 0 (`Frame-Windows.cpp:322-327`), `flush()` = end + submit + begin new command buffer with resource states surviving, present-on-destroy, offscreen ctor waits instead. **`DepthAction::Resume` is `LOAD_OP_LOAD`, not Vulkan suspend/resume.** |
| `GPUView` | Hard, schedule risk | `VK_KHR_wayland_surface`, swapchain recreate on resize/`OUT_OF_DATE`/`SUBOPTIMAL`, per-image semaphores + fences, `framesInFlight` clamp, `DEVICE_LOST` → `onDeviceRestored` (already in `GPUView.h:113-117`), `preTransform = currentTransform`, `renderNativeContent` → offscreen target → BGRA-premultiplied to straight RGBA (README:841-846; tests compensate for premultiplication). |
| `RenderPass` | Moderate–hard (24 entry points) | Descriptor write + barrier per bind; **viewport rejected outright, scissor clamped with outward rounding** (`RenderPass-Apple.mm:104-160`, `ViewportTests`); negative viewport height (§3.5); `vkCmdSetCullMode`/`vkCmdSetFrontFace` on every `setPipeline`. |
| `Buffer` | Moderate | `Device` → `DEVICE_LOCAL` + staging via the open recording; `Streaming` → persistently mapped `HOST_VISIBLE|HOST_COHERENT`; `Storage` always device-local; `read()` waits for submitted work. `StreamingBufferTests` and `SpriteBatchTests` assert `Device::buffersCreated()` counts, so pool recycling semantics must match. |
| `RenderPipeline` | Moderate | Mechanical enum translation. `StencilOp` naming trap: D3D12 `INCR`/`DECR` wrap and `*_SAT` clamp, the opposite of GL/Metal/Vulkan (`RenderPipeline-Windows.cpp:382-386`) — map from the eacp enum. Vulkan accepts `*Color` factors in alpha slots like Metal; no substitution needed. Stencil masks are one pair applied to both faces. |
| `ComputePass` / `ComputePipeline` | Moderate / trivial | Descriptor sets replace root descriptors; explicit barrier after every dispatch (D3D12 does the same, `ComputePass-Windows.cpp:132-142`); `dispatchIndirect` needs only `VK_ACCESS_INDIRECT_COMMAND_READ_BIT`. |
| `Device` | Moderate | Honest feature/format queries (`framebufferColorSampleCounts` for three formats, `textureCompressionBC`, `STORAGE_IMAGE_BIT` per format, `timestampComputeAndGraphics`). Keep the probe-by-trial `DriverQuirks` idea from commit `8d95793` for drivers that lie; add `EACP_VK_SOFTWARE` mirroring `EACP_D3D12_WARP`. |
| `GpuTimestamps` | Moderate | `VkQueryPool(TIMESTAMP)`, `vkCmdWriteTimestamp2`, `timestampPeriod`; D3D12's shape including the frame pair and the self-retire when ticks read zero. Decide whether `FrameTimingTests.cpp:187` treats Linux like Windows (skip when unsupported). |
| `CommandBuffer`, `ShaderLibrary` | Trivial | glslang invocation (~120–160 lines) mirroring `ShaderLibrary-Windows.cpp`; `commitAsync` reuses the 240 Hz `Threads::Timer` poll. |

Design rules carried over from the prior-art survey: conservative barriers hoisted
to pass boundaries (they cannot be issued inside `vkCmdBeginRendering`;
`transitionTextureForUse` already lives there); one combined
`{layout, stage, access}` per resource; `VkPipeline` hash-cached on everything
that is not dynamic state.

### 3.2 The GLSL dialect (`Codegen/`, `Shader/`)

| Work | Lines |
|---|---|
| `Backend::Vulkan` + `emitGlsl()` + GLSL arms at the 27 branch points (→ ~45) | 400–450 |
| `typeName(Backend, ValueType)` — `vec2`/`ivec2`/`bvec2`/`mat4` — threaded to 12 call sites incl. `detail::convertTo` (`ShaderValue.h:2190`) | 70 |
| `UniformLayout.h` std140 arm | 20 |
| `ShaderSource.h`: `ShaderBackend::Vulkan`, `glsl()` factory | 12 |
| `ShaderBuilder-Linux.cpp`, `ShaderLibrary-Linux.cpp`, `CMake/FindGlslang.cmake` | ~170 |
| Move `maxTextureSlots` out of `Windows/D3D12Types.h:35` to a portable header | 5 |
| `ShaderCodegenTests.cpp` + `AtomicTests.cpp` GLSL assertions (parameterise the 11 shared loops' type spellings) | 285–385 |
| GLSL twins of the 8 hand-written test shaders (`GPUSmokeTests` ×2, `ScissorTests`, `BaseVertexTests`, `ViewportTests`, `PipelineStateTests`, `GPUSnapshotTests` ×2) + `Apps/GPU/Triangle/Triangle.glsl`; the `Platform::isWindows() ? hlsl : msl` ternaries become three-way | ~160 |

Binding map (one descriptor set, existing constants reused): uniform block
`layout(std140, set=0, binding=0)` as `UNIFORM_BUFFER_DYNAMIC`; vertex attribute
*i* → `layout(location=i)`; varying *i* → `layout(location=i)`; texture slot *i*
→ `binding=8+i` combined `sampler2D` with immutable sampler from
`graph.textureSampling(i)`; storage buffer *i* →
`layout(std430, binding=24+i) buffer { float buffer_i[]; }` (instance name
omitted so `buffer0[i]` prints byte-identically); compute buffers `binding=i`,
textures `8+i`, uniforms `16` — the Metal indices.

Findings that make this cheaper than feared: std140 agrees with the MSL packing
rules for every type the EDSL allows as a uniform (the existing
`Float2x2`/`Float3x3`/`Bool*` refusals already cover std140's problem types); the
only delta is a `vec3` followed by a scalar, fixed by the same pad-scalar
insertion the HLSL arm already does. `mat4(c0..c3)` needs no `transpose()` and
`*` replaces `mul()`, so the GLSL output is closer to the MSL output than the
HLSL output is. `Array<T,N>` is a function-local const array, outside any block,
so std140 array stride is irrelevant. `atomicAdd` returns the old value, so the
statement shape prints fine with no graph change.

Pitfalls to design around: `input`/`output` are reserved words in GLSL
(`ShaderEmitter.cpp:168,171,1678`); depth textures need `.r` on the sample,
which breaks the invariant stated at `ShaderEmitter.cpp:1288-1293`; `imageStore`
takes signed `ivec2`; `texelFetch` requires a LOD argument; `%` on negative
operands is undefined in GLSL where MSL and HLSL truncate (emit `a - (a/b)*b` or
document); `shared` arrays are globals (the HLSL placement); barrier spelled
`memoryBarrierShared(); barrier();`. If uniform arrays are ever added, std140's
16-byte array stride becomes a fourth "deliberately refuses" entry unless
`VK_KHR_uniform_buffer_standard_layout` (core 1.2) is required.

### 3.3 glslang as a dependency (measured)

Measured with glslang 16.5.0 built Release with `ENABLE_OPT=OFF`,
`ENABLE_HLSL=OFF`, `ENABLE_GLSLANG_BINARIES=OFF`, `GLSLANG_TESTS=OFF`,
`BUILD_EXTERNAL=OFF`, clang on arm64 macOS. GCC on x86_64 Linux will land in the
same range, not identically.

| What | Measured |
|---|---|
| Source checkout, shallow clone | 53 MB; 113k lines core + SPIR-V backend, the 19k-line HLSL front end compiled out |
| External dependencies | None. Configure 1 s. SPIRV-Tools, SPIRV-Headers and googletest are needed only for `ENABLE_OPT` and tests |
| Build | 48 translation units, 15 s wall on 16 cores (roughly a minute on a 4-core CI runner) |
| Output | One static archive, `libglslang.a`, 4.2 MB |
| Added to a stripped Release binary | 1.98 MB (probe linking glslang and compiling a vertex/fragment pair, versus a hello-world) |
| First shader compiled in a process | 89 ms, a one-time built-in symbol table construction |
| Every shader after that | ~0.3 ms per stage; 320–400 SPIR-V words for a Waves-sized shader |

It is a fixed cost per binary, not per shader, and links statically. Metal and
D3D12 get their compiler from the OS, so this would be the first shader compiler
eacp ships itself. Warm the 89 ms hit at device creation or accept it on the
first `prepare()`; decide deliberately rather than discover it in a frame time.

### 3.4 Graphics on Linux — the MVP slice for a `GPUView` in a `Window`

The Windows backend is the blueprint: one HWND per `Window`, no child HWND per
`View`, a DirectComposition visual tree mirroring the view tree, and all input
routed by the portable `View.cpp` hit-tester. Wayland's
surface/subsurface/buffer model maps onto that almost one-to-one, and the
swapchain **is** the `wl_surface`'s buffer queue, so there is no attach step.

| File | Status | Notes |
|---|---|---|
| `View/View.cpp`, `Primitives.cpp`, `ImageOps.cpp`, `Menu.cpp`, `MenuCommands.cpp`, `DisplayLink.cpp` | unchanged | portable |
| `View/View-Linux.cpp` (new) | real, ~250 lines | `Native` = `Rect bounds` + focus flag (+ `wl_subsurface` later). `getHandle()`/`getNativeLayer()` return what `GPUView` parents into. `renderToImage` → `renderNativeContent` directly (the read-back path 29 GPU test files ride on), so it needs no compositor and no display. |
| `Window/Window-Linux.cpp` (new) | real, ~500 lines | `wl_surface` + `xdg_surface` + `xdg_toplevel` + libdecor; `configure` → `setBounds` → `resized()`; honour `Apps::getAppEnvironment().headless` like `Window-Windows.cpp:447`. Can start as a headless stub that builds no surface. |
| Wayland input translation (new) | real, ~400 lines | `wl_pointer`/`wl_keyboard` + xkbcommon → `MouseEvent`/`KeyEvent` → `contentView->dispatchMouseEvent`/`keyDown`, mirroring `CompositionHostWindow-Windows.cpp:596-864`. |
| `Graphics/Keyboard-Linux.cpp` (new) | table, ~250 lines | xkb keysym ↔ `KeyCode` (`Keyboard.h:12-129`). |
| `Helpers/DisplayLink-Linux.cpp` (new) | real, ~120 lines | Thread + `clock_nanosleep` at the output's refresh rate posting via `callAsync` — the `DisplayLink-Windows.cpp:99-101` fallback. Upgrade to `wl_surface.frame` later (per-surface, so needs a `DisplayLink(View&, cb)` overload or `GPUView` driving it directly). |
| `Window/Display-Linux.cpp`, `Image-Linux.cpp`, `Menu-Linux.cpp`, `TrayIcon-Linux.cpp`, `SystemAppearance-Linux.cpp` (new) | stubs | iOS is the precedent for honest no-ops (`Menu-iOS.mm`, `TrayIcon-iOS.mm`); `Display.h:37-39` documents a 1280×800@1 fallback; `Image.h:42-46` documents codec failure. Every `Native` must still be a complete type (`Pimpl` is `make_shared`). |
| `Widgets/TextInput.cpp`, `Layers/*`, `GraphicsContextImpl`, `Font`, `Path`, `TextMetrics`, `GraphicUtils`, `EmbeddedView`, `ImageConversion` | **removed from the Linux source list** | They pull in the 2D text stack. `TextInput` is client-drawn on every platform anyway; IME is the real gap. |
| `Core/Threads/EventLoop-Linux.cpp` | Core change, ~20 lines + an API | `poll(fds, n, timeout)` over `{wakerFd, displayFd}` with Wayland's prepare-read protocol around it. Propose `Threads::addLoopSource(int fd, short events, Callback)` so `eacp-core` never links libwayland. `Timer-Linux.cpp` (one thread per timer) can migrate to `timerfd` later. |
| `Core/App/App-Linux.cpp:30-33` | fix | `openExternalURL` is `assert(false)`; make it `fork/exec xdg-open`. |

Two Windows facts not to replicate: `View::backingScaleChanged()` is never called
on Windows (only `View-macOS.mm:239`), so `GPUView::backingScaleChanged` is dead
there — Wayland fractional-scale changes arrive without a size change, so Linux
must call it. And `Window` construction on Windows forces the whole 2D stack up;
on Linux nothing should.

### 3.5 Coordinate conventions

Vulkan differs from Metal and D3D12 in exactly one axis: NDC y is down with a
positive-height viewport. Depth is already `[0, 1]`, so `Mat4::perspective` and
`ShaderProgram::perspective` transfer unchanged. The fix is a **negative viewport
height** (`VK_KHR_maintenance1`, core since 1.1), applied in
`RenderPass::setViewport`, `clearViewport` and the implicit full-target viewport
at pass begin — and nowhere else:

```cpp
viewport.y = rect.y + rect.h;
viewport.height = -rect.h;
```

With that, `VK_FRONT_FACE_COUNTER_CLOCKWISE` means the same thing as
`MTLWindingCounterClockwise` and `FrontCounterClockwise = TRUE`, and
`CullModeTests`, `ViewportTests` (including `yIsMeasuredFromTheTop`) and
`CoordinateSpaceTests` pass unchanged. The six shader sites that hardcode
clip-y-up (`SpriteRenderer.cpp:22,35`, `GlyphRenderer.cpp:53`,
`PathFillShader.h:23`, `VertexColorShader.h:25`, `CoverageShader.h:34`) stay
untouched. Texel origin, `gl_FragCoord` default and read-back row order already
match. The two wrong fixes are negating `gl_Position.y` (reverses winding;
`CullModeTests` fail unless `frontFace` is also inverted) and negating y in the
projection (breaks the `Mat4.h:16-17` CPU/shader equivalence;
`CubeMap`/`StencilShadows` build matrices on the CPU while `Teapot`/`Maze` build
them in-shader). One investigation argued for a positive height; the Vulkan
spec's signed-area rule in framebuffer coordinates says otherwise, and
`CullModeTests` is the arbiter either way.

### 3.6 Modules above the GPU

| Module | Linux status | Work |
|---|---|---|
| `Sprites`, `GPUWidgets`, `UI` | portable, build the moment `eacp-gpu` does | `defaultUIFontFamily()` returns "Helvetica Neue" on non-Windows (`UI/Common.h:29-35`); needs a Linux branch. |
| `Text` | one platform file | `GlyphRasterizer-Linux.cpp`: FreeType + HarfBuzz + fontconfig. Direct wins: byte clusters via `hb_buffer_add_utf8` (both existing backends maintain a UTF-16→byte map that becomes unnecessary), 4-phase subpixel via `FT_Set_Transform`, variable fonts via `FT_Set_Var_Design_Coordinates`. Must port: CSS weight/width/slant matching (`GlyphRasterizer-Apple.mm:265-286, 452-471`), `opsz` pinned to point size, script/direction itemization (the largest new piece — HarfBuzz needs a run splitter), per-run `.notdef` fallback re-shaping. Report unhinted advances (`FT_LOAD_NO_HINTING`) while rasterising with `FT_LOAD_TARGET_LIGHT`, never LCD AA. `defaultMonospaceFamily()` returns "Menlo" on non-Windows (`Text/Font.h:71-77`) — every font test self-skips when it cannot resolve, so the whole Text suite silently skips until this is fixed. |
| `SVG` | split the target | Parse layer + `SVGComponent` are portable over `eacp-ui`; only `SVGBuilder.cpp` needs native `ShapeLayer`/`TextLayer`. `Tests/SVG` already models the split. |
| `WebView` | bridge now, native never/late | `eacp-webview-bridge` is portable today and only inside the graphics gate by directory placement. |
| `Camera`, `Video`, `CameraView`, `VideoView` | deferred | Stub shapes exist; V4L2/PipeWire later if ever. |

## 4. Build system and CI

**Gate.** Replace the single predicate at `Lib/eacp/CMakeLists.txt:5` (mirrored
verbatim at `Apps/CMakeLists.txt:6` and `Tests/CMakeLists.txt:14,18,38,46`) with
capability variables set once and read by all three trees:

```cmake
option(EACP_LINUX_GRAPHICS "Build the Wayland/Vulkan graphics backend on Linux" OFF)
set(EACP_HAS_DRAW    (APPLE OR WIN32 OR (LINUX AND EACP_LINUX_GRAPHICS)))   # Graphics, GPU, GPUWidgets, Text, Sprites, UI, SVG
set(EACP_HAS_CAPTURE (APPLE OR WIN32))                                        # Camera, CameraView, Video, VideoView
set(EACP_HAS_WEBVIEW (APPLE OR WIN32) AND EACP_BUILD_WEBVIEW)                 # native WebView; the bridge is unconditional
```

`Graphics/CMakeLists.txt:108-110`'s `elseif (UNIX)` becomes the real Linux
branch. Check `Graphics/CMakeLists.txt:117`, which unconditionally adds
`IconTool`.

**Dependencies.** Follow the libcurl precedent (`Network/CMakeLists.txt:45-46`)
for system libraries and the `CMake/FindMiro.cmake` shape for CPM:

| Dependency | Mechanism | Notes |
|---|---|---|
| Vulkan-Headers, volk, VMA, glslang | CPM | glslang `16.5.0` with `ENABLE_OPT OFF`, `ENABLE_HLSL OFF`, `ENABLE_GLSLANG_BINARIES OFF`, `GLSLANG_TESTS OFF`, `BUILD_EXTERNAL OFF`; link `glslang-default-resource-limits`. All four are cross-platform, so `emitGlsl` + glslang can also be compiled on macOS/Windows for testing. |
| wayland-client, xkbcommon, libdecor-0, (fontconfig, harfbuzz later) | `pkg_check_modules(... IMPORTED_TARGET)` | `wayland-protocols` is XML; run `wayland-scanner` via `add_custom_command` (cf. `eacp-icon-tool`, `TargetSetup.cmake:195-200`). |
| freetype | `find_package(Freetype)` | CMake ships the module. |

apt for the CI lane (Ubuntu 24.04): `mesa-vulkan-drivers vulkan-tools
vulkan-validationlayers libwayland-dev wayland-protocols libxkbcommon-dev
libdecor-0-dev libfreetype-dev libfontconfig-dev libharfbuzz-dev
fonts-dejavu-core fonts-noto-color-emoji` (fonts are needed or the Text suite
silently skips). Mirror into the `Dockerfile` behind `ARG EACP_GRAPHICS`.

**Lane shape.** Add graphics as a matrix dimension, keeping the existing
GCC/Clang lanes proving `-DEACP_BUILD_GRAPHICS=OFF`:

```yaml
- name: Linux Clang Graphics
  os: ubuntu-24.04
  cmake-flags: '-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DEACP_LINUX_GRAPHICS=ON'
env:
  EACP_HEADLESS: "1"
  VK_DRIVER_FILES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
```

Three CI traps: NanoTest runs every test binary with `--list-tests` **at build
time** (`NanoTestAddTests.cmake:4-13`, hard `FATAL_ERROR`), so the ICD and fonts
must exist before `cmake --build`; every GPU test self-skips with a bare `return`
when `Device::shared().isValid()` is false, which ctest scores as a pass, so the
lane needs one non-skipping "a device was obtained" assertion gated by a CI env
var, plus a `vulkaninfo --summary` step; and `EACP_CI_BUILD` turns unity builds
on, so new `*-Linux.cpp` files can collide on anonymous-namespace names or
`<vulkan/vulkan.h>` macros — build the lane both ways once.

## 5. Staged plan

| Stage | Deliverables | Unlocks | Size |
|---|---|---|---|
| **0 — no Vulkan, no Linux windowing** | `emitGlsl` + glslang, developed and tested on macOS/Windows; carve a device-free `GPUCodegenTests` target (`ShaderCodegenTests.cpp` + the codegen half of `AtomicTests.cpp`) and enable it on Linux; narrow `GPU/Common.h` to the Primitives/Image headers it uses; move `eacp-webview-bridge` + `ScriptHostTests` out of the graphics gate; fix Linux `defaultMonospaceFamily`/`defaultUIFontFamily`/`openExternalURL`. | ~85 real Linux test cases; the whole dialect verified by `spirv-val` before any device exists | ~1,200 lines |
| **1 — headless Graphics on Linux** | `EventLoop-Linux` fd source; `View-Linux.cpp` (software tree, `renderToImage` → `renderNativeContent`); headless `Window-Linux.cpp`; stubs for Display/Image/Menu/Tray/SystemAppearance; timer `DisplayLink-Linux`; Linux source list excluding TextInput/Layers/2D; the gate split; `EACP_LINUX_GRAPHICS` option. | `eacp-graphics` and therefore `eacp-gpu` link on Linux | ~800 lines |
| **2 — Vulkan compute + buffers** | `VulkanShared`/`VulkanContext`, `Device`, `Buffer`, `ShaderLibrary`, `ComputePipeline`, `ComputePass`, `CommandBuffer`, `GpuTimestamps`; `EACP_VK_SOFTWARE`; CI lane on lavapipe with device-presence assertion. | `Apps/GPU/PathBench` (already headless); `GPUSmokeTests`, `AtomicTests`, `BufferRangeTests`, `IndirectDispatchTests`, `SharedMemoryTests`, `StorePlacementTests`, `MultiDeviceTests`, `TextureUpdateTests` | ~3,500 lines |
| **3 — Vulkan render, offscreen** | `Texture`, `RenderPipeline`, `RenderPass`, `Frame(OffscreenTarget)`, `Texture::read`, `GPUView::renderNativeContent` via an offscreen target (no swapchain). | The remaining ~25 pixel-comparison GPU tests (`CullModeTests`, `ViewportTests`, `MultisampledTargetTests`, `CompressedTextureTests`, `StencilTests`, `DepthActionTests`, ...), `Tests/GPUWidgets` (56), `Tests/UI` (140) with a stub font — all headless | ~4,000 lines |
| **4 — Wayland window + swapchain** | Real `Window-Linux.cpp` (xdg-shell, libdecor), input translation, `Keyboard-Linux`, `Display-Linux`, `wl_surface.frame`-driven `DisplayLink`, `GPUView-Linux.cpp` swapchain with resize/`OUT_OF_DATE`/`DEVICE_LOST`. | `Apps/GPU/Teapot`, `Maze`, `CubeMap`, `StencilShadows` (all EDSL shaders — GLSL for free); `Tests/Graphics` subset | ~2,000 lines |
| **5 — Text** | `GlyphRasterizer-Linux.cpp` (FreeType + HarfBuzz + fontconfig), fonts on the runner. | `eacp-text`, `eacp-sprites`, `eacp-ui`, `Tests/Text` (104), `Apps/UI/*`, `Apps/GPU/GlyphAtlas`, `VariableFont` | ~800–1,200 lines |
| **6 — later** | SVG target split; clipboard via `wl_data_device`; portal file dialogs; IME (`text-input-v3`); xcb fallback; Cairo/Pango `Context` if ever wanted; SPIR-V disk cache; `VK_EXT_descriptor_heap` when it is broad. | `Tests/SVG` (96), `Apps/SVG`, the `Apps/Graphics` menu/tray apps | — |

Stages 0–3 need no display server and no Wayland code, and stage 0 needs no
Linux machine at all. Stage 4 is the largest schedule risk and the one most
decoupled from the GPU work.

## 6. Risks and traps (collected)

- **Silent green**: self-skipping tests, missing fonts, and a missing ICD all
  report as passes. Assert device presence and font resolution in the CI lane.
- **`Apps/GPU/Triangle` is a trap**: the only app with hand-written per-backend
  shader files; it needs a `Triangle.glsl`. Use `Teapot`/`Maze` as the first
  visual smoke test instead.
- **Eight test shaders pick with `Platform::isWindows() ? hlsl : msl`**:
  non-Windows silently means Metal today.
- **`maxTextureSlots` lives in `Windows/D3D12Types.h:35`** and the emitter needs
  it.
- **`Buffer::stage` records onto the open frame**, texture uploads do not
  (`Texture-Windows.cpp:541-634`); unify on the buffer behaviour.
- **MSAA samples must be kept, not just resolved**, or `DepthAction::Resume` and
  mid-frame copies break at 4× (`Frame-Apple.mm:333-344`).
- **`DepthAction::Resume` ≠ Vulkan suspend/resume.**
- **Barriers cannot be issued inside `vkCmdBeginRendering`**; hoist to pass
  boundaries.
- **`maxMemoryAllocationCount`** rules out the committed-resource model; hence
  VMA.
- **Descriptor buffers are unsupported on lavapipe** and already being
  superseded.
- **Dawn's dynamic-rendering quirk list** includes Intel ≤ Gen9 (Skylake iGPUs
  still in service).
- **HiDPI**: Linux must call `View::backingScaleChanged()`; Windows never does
  and gets away with it only because `WM_SIZE` follows.
- **NanoTest discovery runs binaries at build time.**
- **Unity builds under `EACP_CI_BUILD`** can collide new platform files.
- **`Graphics/CMakeLists.txt:117`** adds `IconTool` unconditionally.
- **README/CLAUDE.md** both restate the gate (`README.md:65-81`,
  `CLAUDE.md:14-22`) and need matching edits, with 🚧 rows during rollout.

## 7. Open decisions

- **D1 — Shader route.** GLSL + glslang (recommended) vs. HLSL + prebuilt DXC.
  The former costs ~700 lines of emitter/compile work, ~2 MB per binary and one
  source of truth for bindings; the latter costs zero emitter work but a 515 MB
  binary blob, a shift-flag table, and a Linux-only FXC/DXC divergence class.
- **D2 — Loader linkage.** volk + `dlopen` (recommended; no new apt build
  dependency) vs. `find_package(Vulkan)` + link `libvulkan`.
- **D3 — Dynamic-rendering policy.** Sole path with Dawn's quirk list making
  `Device::isValid()` false on Intel ≤ Gen9 / Mali-G68 / PowerVR (simplest,
  excludes Skylake iGPUs), or sole path with no list and add quirks only when
  reproduced (matches the repo's "measured, not reasoned" and `DriverQuirks`
  ethos; recommended). A second `VkRenderPass` path is not recommended.
- **D4 — Wayland-only first** (recommended) vs. Wayland + xcb from the start.
  XWayland covers X11 sessions at the cost of blurry fractional scaling.
- **D5 — Text scope for Linux.** Full FreeType/HarfBuzz/fontconfig with
  itemization and fallback (stage 5, recommended), or a reduced first cut that
  skips script itemization and colour emoji.
- **D6 — `EACP_LINUX_GRAPHICS` default.** OFF until stage 3 lands, ON afterwards.
