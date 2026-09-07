# Linux graphics support — investigation and plan

Written 2026-09-05 against `b0de675`, from five read-only investigations of the
tree (GPU backend contract, shader codegen, Graphics layer and event loop,
dependent modules/tests/CI, external Vulkan-on-Linux research). Line counts
are estimates from reading the Metal and D3D12 backends, not commitments.

## 0. Progress

**Stage 0 — landed 2026-09-05** (verified: macOS 1494 tests, Linux GCC 557
tests, plain and `EACP_CI_BUILD` unity, plus Linux Clang).

- `emitGlsl` GLSL 450 dialect in the one walker; `ShaderSource::glsl`,
  `ShaderBackend::Vulkan`; `Codegen/ShaderBindings.h` holds `maxTextureSlots`
  and the Vulkan binding map. Stage-macro contract: one source starting
  `#version 450`, vertex-only code under `#ifdef EACP_VERTEX`, fragment-only
  under `#ifdef EACP_FRAGMENT`, compute a single `main`.
- `eacp-spirv` (`GPU/Spirv/`, glslang 16.5.0 via CPM behind
  `EACP_BUILD_SPIRV`): `Spirv::compileGlsl(Stage, source)`; only tests and,
  later, the Vulkan backend link it. `GPUCodegenTests` and the hand-written
  twins in `GPUTests` compile every emitted GLSL shader with it on every lane.
- `eacp-gpu-codegen` carved out of `eacp-gpu`; `GPU/Common.h` narrowed to
  `Primitives.h`; `ShaderBuilder-Linux.cpp` emits GLSL; `GPUCodegenTests`
  (66) and `ScriptHostTests` run on Linux. Gate is now `EACP_HAS_DRAW` /
  `EACP_HAS_CAPTURE` / `EACP_HAS_WEBVIEW` plus `EACP_LINUX_GRAPHICS` (OFF).
- `eacp-webview-bridge` no longer links `eacp-graphics`. Linux
  `openExternalURL` runs `xdg-open` detached; DejaVu font defaults.

**Stage 1 — landed 2026-09-05** (verified: Linux with `EACP_LINUX_GRAPHICS=ON`
665 tests under GCC, Clang and `EACP_CI_BUILD` unity; option off 561; macOS
unchanged at 1494).

- `eacp-graphics` builds and links on Linux headless: `View-Linux.cpp`
  (`Native` = bounds + focus; `getHandle()`/`getNativeLayer()` return the
  `Native*` for a later `GPUView`; `renderToImage` → `renderNativeContent`),
  headless `Window-Linux.cpp`, timer `DisplayLink-Linux.cpp`, stubs for
  Display/Image/Menu/TrayIcon/SystemAppearance/Layer. No 2D `Context`, `Path`,
  `Font`, layers or `TextInput` on Linux, so no app links there yet.
- `Threads::addLoopSource(fd, events, cb)` / `removeLoopSource(fd)` in
  `Core/Threads/EventLoop-Linux.h`, polled alongside the waker; four tests.
- Gate refined: `EACP_HAS_GPU` (GPU runtime, GPUWidgets, Sprites; Linux joins
  at stage 2) and `EACP_HAS_TEXT` (Text, UI, SVG and their apps; stage 5).
- `GraphicsTests` runs 104 cases on Linux; `ImageTests`, `RenderToImageTests`
  and `RoundedRectTests` are left out (codec, paint context, `Path`).
- HiDPI seam for stage 4: `Graphics::notifyBackingScaleChanged(View&)` in
  `View/View-Linux.h`; nothing calls it yet.

**Stage 2 — landed 2026-09-05** (verified: Linux with `EACP_LINUX_GRAPHICS=ON`
757 tests under GCC, Clang and `EACP_CI_BUILD` unity+PCH, and zero validation
messages under `EACP_VK_VALIDATION=1`; option off 565; macOS 1586, which is the
tree's 1581 plus the device-presence case and four codegen cases).

- The Vulkan compute half. `GPU/Vulkan/VulkanContext.h` + `VulkanTypes.h` +
  `VulkanContext-Linux.cpp` split process-wide `VulkanShared` (loader,
  instance, physical-device selection against a named 1.3 feature floor,
  `VkDevice`, queue, VMA allocator, compute descriptor-set and pipeline layouts
  built from `Codegen/ShaderBindings.h`) from per-`Device` `VulkanContext`
  (command-pool ring, timeline semaphore, `openRecording`, upload arena,
  constant ring, staging/readback pools, per-recording descriptor pools,
  deferred release stamped at submit). Real: `Device`, `Buffer`,
  `ShaderLibrary`, `ComputePipeline`, `ComputePass`, `CommandBuffer`,
  `GpuTimestamps`. Honest placeholders reporting invalid: `Texture`,
  `RenderPipeline`, `RenderPass`, `Frame`, `GPUView`.
- Dependencies: `CMake/FindVulkanBackend.cmake` CPM-fetches Vulkan-Headers and
  volk (`vulkan-sdk-1.4.313.0`, matching tags) and VMA `v3.4.0` into one
  `eacp-vulkan` target with `VK_NO_PROTOTYPES`; nothing links `libvulkan`
  (`${CMAKE_DL_LIBS}` only), and macOS/Windows fetch none of it.
- Gate: `EACP_HAS_GPU` is now on for `LINUX AND EACP_LINUX_GRAPHICS`, so
  `eacp-gpu`, `eacp-gpuwidgets`, `eacp-sprites`, `Apps/GPU` and their tests
  build there. `EACP_HAS_TEXT` still gates the `Apps/GPU` examples that paint a
  2D overlay (Blending, Instancing, StencilShadows, CubeMap, DepthSampling,
  PathQuality, PathStroke).
- `EACP_VK_SOFTWARE`, `EACP_REQUIRE_GPU` and `EACP_VK_VALIDATION`; a
  `Linux Clang Graphics` CI lane on lavapipe with `vulkaninfo --summary` before
  the build; `Tests/GPU/DevicePresenceTests.cpp` is the non-skipping "a device
  was obtained" assertion.
- Moved out of the portable test lists, each awaiting the render half:
  `GPUSmokeTests` and `TextureUpdateTests` (need `Texture`),
  `ShaderCompileTests` (needs `RenderPipeline`), `MultiDeviceTests` (asserts two
  Devices have two queues; lavapipe has one), and three cases carved out of
  `Tests/GPUWidgets/PathTests.cpp` into `ShaderPipelineTests.cpp`. Everything
  else in `Tests/GPUWidgets` stayed by self-skipping on the mask `Texture`
  instead of on the device.
- A `Graphics/Keyboard-Linux.cpp` stub, without which `Apps/GPU/Maze` did not
  link.
- The stage-1 emitter follow-ups, in all three dialects: a fragment read of a
  vertex input with no varying is promoted to an implicit varying appended
  after the declared ones (both compile-check exclusions are gone); integer
  varyings are emitted `flat`/`nointerpolation`/`[[flat]]`, which glslang
  requires; a `Bool` varying is refused by `static_assert`, GLSL having no
  boolean stage I/O; GLSL `%` on signed integers takes the truncating form MSL
  and HLSL use. `UniformLayout.h` gained `uniformBlockSize`/`std140BlockSize`,
  and `std140BlockAlignment` is the one constant the Vulkan constant ring
  rounds with. The §3.2 pitfall list was audited: every entry is handled and
  pinned by a codegen test. `GPUCodegenTests` 66 → 70.
- `Primitives/Path-Linux.cpp`: `Graphics::Path` on Linux as recorded geometry
  (`PathGeometry` in `Path-Linux.h`: move/line/quad/cubic/close, rects,
  rounded rects and ellipses decomposed into cubics, the radius clamped by
  `clampedCornerRadius`). `RoundedRectTests` now runs everywhere and
  `PathTests-Linux` pins the record; `GraphicsTests` is 118 cases on Linux.

**Stage 3 — landed 2026-09-06** (verified after the develop merge: Linux with
`EACP_LINUX_GRAPHICS=ON` 993 tests under GCC, `GPUTests` 251 and
`GPUWidgetsTests` 57 with zero validation messages under
`EACP_VK_VALIDATION=1`; option off 567; macOS 1506 with `GPUTests` 252 and
`GPUWidgetsTests` 57. The `EACP_CI_BUILD` unity+PCH build was checked just
before the merge, at 991, and not again after it.)

- The render half of the Vulkan backend, off-screen. `Texture-Linux.cpp`:
  `VkImage` + view through VMA, cube as a six-layer image, CPU mip chains and
  supplied chains, BC1/2/3/7 uploads, region update and read through the
  context's arena and readback pool, the multisampled companion, the depth
  companion (`D32_SFLOAT` / `D32_SFLOAT_S8_UINT`) with a depth-only read view
  and a resolved-depth image for a sampleable depth on a multisampled target,
  and lifetime layout tracking (`VulkanTextureData`, `ImageUse`,
  `recordImageBarrier`). `RenderPipeline-Linux.cpp`: a dynamic-rendering
  `VkGraphicsPipeline` with cull mode and front face baked in, `VIEWPORT`,
  `SCISSOR` and `STENCIL_REFERENCE` dynamic. `Frame-Linux.cpp` and
  `RenderPass-Linux.cpp`: `vkCmdBeginRendering` per pass with `DepthAction` as
  load/store ops, resolve through the attachment's resolve fields (`AVERAGE`
  for colour, `SAMPLE_ZERO` for depth), the negative viewport height of §3.5,
  scissor clamped with outward rounding, one descriptor set per draw elided
  when nothing changed, `flush()` as submit-and-reopen, an off-screen destructor
  that waits. `GPUView-Linux.cpp`: `renderNativeContent` through a real
  render-target `Texture` and `Texture::read`, premultiplied BGRA to straight
  RGBA as on Windows. The drawable `Frame` constructor and the swapchain half of
  `GPUView` stay placeholders for stage 4.
- The four `VkSampler`s in `VulkanShared` (`getSampler`, `Device::nativeSampler`)
  and the render descriptor-set layout (`getRenderLayouts`: one dynamic uniform
  block, eight combined image samplers, eight storage buffers, every binding
  visible to both stages, all partially bound). The stage-2 compute-texture
  follow-up is resolved by per-module SPIR-V reflection
  (`spirvTextureBindings`): a kernel that declares a texture builds its own
  descriptor-set layout naming each slot as the type the module declared.
- Barrier design: resting layouts between passes (colour images sampleable,
  `GENERAL` for `computeWrite`, the multisample companion in
  `COLOR_ATTACHMENT_OPTIMAL`), attachments moved in at pass begin and out at
  pass end, one hoisted global barrier before every `vkCmdBeginRendering`, and
  no barrier in any bind. An upload made while a pass is open takes a recording
  of its own (`VulkanContext::getRecordingForCopy`), which fixed 46 validation
  errors found on the way.
- Tests: the `APPLE OR WIN32` list in `Tests/GPU/CMakeLists.txt` is gone; every
  file but `TextureInteropTests.mm` is portable. `MultiDeviceTests` guards only
  the queue-distinctness assertion on Linux, `FrameTimingTests` treats a
  backend without pass timings as Windows is treated. `TextureCreationTests.cpp`
  is new and portable; `PathRasterizerTests` gains the mirror assertion that
  `PathRasterizer::getCoverage().isValid()` under `EACP_REQUIRE_GPU=1`, so the
  coverage comparisons in `GPUWidgetsTests` can no longer report green while
  skipping. `ShaderPipelineTests::fillShaderCodegen` has a GLSL arm.
- `origin/develop` (273cd99, the EA-container and int-sized-interface move) was
  merged beneath this stage on the same day and the Linux backend ported to
  the new signatures.

Follow-ups found on the way, not yet done:

- The `maxTextureSlots` move out of `D3D12Types.h` is unverified on a Windows
  compiler until CI runs, and so is everything in the develop merge that touched
  `*-Windows.cpp`.
- `RenderPipeline::nativeDepthState()` is non-null only for `depth`, as on
  D3D12; Metal answers for `depth || stencil`. Its only caller is
  `RenderPass-Apple.mm`, so nothing reads the wrong answer on Linux or
  Windows; the header's "null when the pipeline tests neither" is what those
  two contradict for a stencil-only pipeline.
- `wrapPixelBuffer` is invalid on Linux, as on Windows, there being no capture
  backend to produce one.
- D6 is still open: `EACP_LINUX_GRAPHICS` stays `OFF` by default until decided.

**Stage 4 — landed 2026-09-06** (verified: Linux with `EACP_LINUX_GRAPHICS=ON`
1017 tests headless under GCC with zero validation messages under
`EACP_VK_VALIDATION=1`, and the same 1017 inside a headless Weston session with
`EACP_REQUIRE_DISPLAY=1` — the 8 `Present` cases and the 9 `WaylandWindowTests`
cases running for real, `GPUTests` 259 and `WaylandWindowTests` 9 under
validation with zero messages; `EACP_CI_BUILD` unity+PCH under Clang 1017; option off
568; macOS unchanged at 1506.)

- Written as two parallel slices meeting at one contract header,
  `Graphics/View/View-Linux.h`: a `ViewSurface` record per presenting view —
  opaque `wl_display*`/`wl_surface*`, pixel size and scale, and the
  `onAvailable`/`onLost`/`onResized`/`onRepaint`/`onFrameDone` hooks plus
  `requestFrameCallback` — obtained with `requestViewSurface(View&)`. The GPU
  module knows Wayland as those two forward-declared pointers
  (`vulkan_wayland.h` needs no more) and neither links nor includes it.
- The Wayland half of `eacp-graphics`. `Window/WaylandDisplay-Linux.{h,cpp}`:
  one connection per process, every registry global optional, outputs, the
  libdecor context, a surface-to-owner map, shm buffers, and the loop source —
  pumped through a new four-argument `Threads::addLoopSource(fd, events, cb,
  prepare)` whose `prepare` runs before every `poll()` to flush requests and
  dispatch what Mesa's WSI left queued (`Tests/Core/EventLoopSourceTests-Linux`
  pins the ordering). `Window-Linux.cpp`: `wl_surface` + libdecor frame,
  configure → constraints → `resized`, a viewport-stretched 1×1 shm background
  with an opaque region, hide as unmap, `wp_fractional_scale_v1` with
  `preferred_buffer_scale` and the output's integer scale as fallbacks,
  activation from keyboard focus. `View-Linux.cpp`: a desynchronised
  `wl_subsurface` per presenting view, created when the view is in a mapped
  window and effectively visible and torn down (`onLost` first) when any of
  that stops, positioned by a parent commit, sized by `wp_viewport` or
  `set_buffer_scale`, `repaint()` coalesced into `onRepaint`, frame callbacks
  relayed. `Window/WaylandInput-Linux.{h,cpp}`: seat, frame-grouped pointer
  with click counting and wheel, xkbcommon keyboard with repeat, cursor theme,
  pointer lock + relative pointer for `setMouseLocked`. `Keyboard-Linux.{h,cpp}`:
  the evdev ↔ `KeyCode` table and polled state; `Display-Linux.cpp` from the
  first `wl_output`; `DisplayLink-Linux.cpp` paced at the output's refresh rate.
  `CMake/FindWayland.cmake`: pkg-config for wayland-client, wayland-cursor,
  xkbcommon and libdecor-0, `wayland-scanner` over six protocol XMLs into one
  `eacp-wayland` target, linked PRIVATE.
- The swapchain half of the Vulkan backend. `VulkanShared` enables
  `VK_KHR_surface` + `VK_KHR_wayland_surface` and `VK_KHR_swapchain` where
  offered (`supportsPresentation()`; `VK_USE_PLATFORM_WAYLAND_KHR` on
  `eacp-vulkan` so volk loads the entry points); `VulkanContext::submit` takes a
  `SubmitSync` pair of binary semaphores beside the timeline; a `presentable`
  `VulkanTextureData` rests at `PRESENT_SRC_KHR` so `RenderPass::end` is
  untouched, and `imageAcquired` sources the first barrier at the colour-output
  stage so it orders behind the acquire; `VulkanDrawable` is what the drawable
  `Frame` receives, and `~Frame` submits with the semaphores and presents with
  no wait. `GPUView-Linux.cpp`: surface and swapchain over the view's record
  (`B8G8R8A8_UNORM`, mailbox else FIFO, opaque alpha, `minImageCount + 1`,
  `framesInFlight` clamped, one acquire semaphore per slot and one
  render-finished per image, CPU throttle on the context timeline, bounded
  acquire timeout), rebuilt on `onResized`/`OUT_OF_DATE`/`SUBOPTIMAL`/`NOT_READY`,
  `onRepaint` and `renderNow` as the on-demand path, continuous mode paced by
  `onFrameDone` with `setMaxFps` as a divider plus a cap timer, companions
  shared with `Texture` through `createVulkanMultisampleCompanion` /
  `createVulkanDepthCompanion` / `releaseVulkanCompanions`. A frame whose
  `render()` opens no pass still leaves the image presentable.
- Tests: `Tests/Graphics/WaylandWindowTests-Linux.cpp` (own `main`, not
  headless: configure, sizes, `ViewSurface` availability/loss/resize, frame
  callback round trip, `primaryDisplay()`), `KeyCodeTests-Linux.cpp`,
  `Tests/GPU/PresentTests-Linux.cpp` (continuous frames, `renderNow`/`repaint`,
  subview resize reaching the swapchain, hide/show, snapshot while presenting,
  teardown and a second window; one case runs headless). All self-skip without
  a compositor and fail instead under `EACP_REQUIRE_DISPLAY=1`.
- Infrastructure: `Scripts/with-weston` runs a command inside a headless
  Weston session; the `Dockerfile` installs the Wayland toolchain and Weston
  and copies the script in as `with-weston`; the CI graphics lane installs the
  same packages and runs ctest under it with `EACP_HEADLESS=0` and
  `EACP_REQUIRE_DISPLAY=1`.

Follow-ups found on the way, not yet done:

- Input has no compositor coverage: Weston's headless backend advertises no
  `wl_seat`, so `WaylandInput-Linux.cpp` never executes on CI; the table and
  the routing are unit-tested, the translation between them is not.
- Device loss is terminal: `VK_ERROR_DEVICE_LOST` tears the swapchain down and
  stops; there is no `VkDevice` rebuild and `onDeviceRestored` never fires.
- A surface offering neither `B8G8R8A8_UNORM` nor `SRGB_NONLINEAR` gets its
  first format while `RenderPipelineDescriptor::colorFormat` defaults to
  `BGRA8Unorm`; no such surface has been seen.
- Not expressible on Wayland and documented rather than faked: window
  position (`getPosition`/`setPosition` keep the app's value), `toFront`
  (maps, cannot raise), per-window icon, `showInactive`, `alwaysOnTop`,
  `visibleOnAllWorkspaces`, `ignoresMouseEvents`, `cornerRadius`; aspect
  ratio is width-driven because libdecor's configure carries no resize edge;
  non-precise wheel deltas are reported in lines, as `View.h` documents,
  where Windows reports `WHEEL_DELTA` units.
- Weston 13 headless offers no `wp_fractional_scale_manager_v1` and no
  `zxdg_decoration_manager_v1`, so CI exercises the integer-scale path and
  libdecor's built-in fallback plugin only.
- Stages 5 and 6 still stand as written below; D6 remains the user's.

**Stage 5 — landed 2026-09-06** (verified: Linux with `EACP_LINUX_GRAPHICS=ON`
1419 tests headless under GCC with zero validation messages under
`EACP_VK_VALIDATION=1`, and the same 1419 inside a headless Weston session with
`EACP_REQUIRE_DISPLAY=1` and `EACP_REQUIRE_FONTS=1`; `EACP_CI_BUILD` unity+PCH
under Clang 1419; option off 570; macOS 1538 with WebView off, which is the
1531 the develop merge left plus the seven cases below.)

- Written as two parallel slices plus one fix found by running the union. The
  first is `Text/GlyphRasterizer-Linux.cpp`, FreeType + HarfBuzz + fontconfig
  in one file: a `FontRequest::family` resolves through the process-wide
  memory-font registry first (family or PostScript name, case-insensitive),
  then `FcFontMatch`, with a `FC_POSTSCRIPT_NAME` retry before a substitute is
  accepted; `resolvedFamily()` is the matched family, so a name the machine
  lacks is valid and says what it became. The family's faces come from
  `FcFontList` (`FcWeightToOpenType` for the weight, `FC_WIDTH`, `FC_SLANT`, a
  variable master's weight range read as "any weight", named instances
  skipped) and are matched by the Apple file's rule — width before slant
  before weight, `weightDistance` copied verbatim. One sized `FT_Face` and one
  hb font per (weight class, slant), cached; `FT_Set_Var_Design_Coordinates`
  supplies `wght` (clamped to the axis), `ital` or else `slnt` for a missing
  italic, and pins `opsz` to the point size rather than the pixel size;
  synthetic bold and oblique (`hb_font_set_synthetic_bold`/`slant` beside
  `FT_Outline_EmboldenXY` and a shear) only when neither a sibling face nor
  an axis supplies them. Shaping itemizes first — by script through
  `hb_unicode_script` with Common/Inherited/Unknown joining the neighbouring
  script, then by font: the base cmap, else the first `FcFontSort` entry
  whose charset has the codepoint, a colour font preferred for emoji
  presentation, each fallback numbered per rasterizer from 1 in order of first
  use — then hands HarfBuzz one run per change of either, the whole string in
  the buffer with the item as its range so clusters are byte offsets, the
  direction from the script. Advances are HarfBuzz's unhinted ones
  everywhere; outlines are light-hinted, shifted by the subpixel phase before
  `FT_Render_Glyph`, and `lightText` gets the same mask. A CBDT colour strike
  (Noto Color Emoji is one 109-px strike) is selected with `FT_Select_Size`,
  shaped through HarfBuzz's own OpenType metrics rather than hb-ft, box
  filtered to the requested size and un-premultiplied to straight RGBA.
  `registerMemoryFont` copies the bytes into a registry that lives for the
  process, reads family, PostScript name, OS/2 weight, style and axes through
  FreeType, and reports a face registered twice as it was. One `FT_Library`,
  one `FcInit`, one mutex around every call into the three libraries.
  `CMake/FindLinuxText.cmake`: pkg-config `freetype2`, `harfbuzz`,
  `fontconfig` into one `eacp-linux-text` target that `eacp-text` links
  PRIVATE. Tests: `Tests/Text/FontPresenceTests.cpp` fails under
  `EACP_REQUIRE_FONTS=1` unless the stock families resolve to themselves, Han
  falls back to another face and U+1F600 comes back as a colour glyph;
  `ShapingTests.cpp` names "DejaVu Sans" for kerning, ligatures, weights and
  widths on Linux (it kerns "AV", ligates "fi", and `fonts-dejavu-extra` adds
  ExtraLight and the Condensed cuts) and "Inter" for the optical-size case,
  which self-skips there. `TextTests` is 100 on Linux, none of them skipping
  except that one.
- The second slice is the gate. `EACP_HAS_TEXT` is on for `LINUX AND
  EACP_LINUX_GRAPHICS`, and a sixth capability variable, `EACP_HAS_CONTEXT`
  (`EACP_HAS_DRAW AND (APPLE OR WIN32)`), names the platform's own 2D tier —
  `Graphics::Context`, `Font`, `TextMetrics`, `TextInput`, `EmbeddedView`, the
  retained `ShapeLayer`/`TextLayer` and their views, the image codecs — and
  gates what stands on it: the layer sources and `IconTool` in Graphics (the
  old `APPLE OR WIN32` tests, now named), `ImageTests` and
  `RenderToImageTests`, `SVGBuilder.cpp` with `SVGParser.cpp` (`SVG::parse`
  calls the builder and its `ParseResult` destroys native layers, so on Linux
  it is a declaration with no definition and the documented path is
  `parseXML` + `SVGComponent::setDocument`), `Apps/Graphics`, `Apps/Plugins`,
  `Apps/SVG`, `Apps/UI/SVGDocument` and the seven `Apps/GPU` overlay
  examples. Newly built on Linux: `eacp-text`, `eacp-ui`, the component half
  of `eacp-svg`, `TextTests`, `UITests`, `SVGTests`, `SVGImageTests`,
  `Apps/GPU/GlyphAtlas` and `VariableFont`, and six `Apps/UI` examples; the
  macOS and Windows target sets are unchanged. CI's graphics lane installs
  `libfreetype-dev libharfbuzz-dev libfontconfig-dev fonts-dejavu-core
  fonts-dejavu-extra fonts-noto-color-emoji fonts-droid-fallback`, shows what
  fontconfig resolves before the build, and sets `EACP_REQUIRE_FONTS=1` on
  the Weston test step; the `Dockerfile`, `README.md` and `CLAUDE.md` say the
  same.
- Running `UITests` on lavapipe for the first time found the GLSL dialect
  printing a scalar beside a vector verbatim — `length(max(0.f, q))` in
  `UI/Render/ShapeBatch.cpp`, which MSL converts and HLSL promotes but GLSL
  has no overload for — so sixteen UI and SVG pixel tests drew nothing.
  `ShaderEmitter.cpp` now broadcasts every scalar argument of a mixed call to
  one of the eight genType builtins (`min`, `max`, `clamp`, `mix`, `step`,
  `smoothstep`, `pow`, `atan2` — exactly the set `ShapedBeside` admits a
  mixed shape for) through the call's vector constructor, in the Vulkan
  dialect only; integer/float mixes are already a C++ compile error in the
  EDSL. Pinned by `GPU/codegenGlslScalarBesideVector`, and the gap that let
  it through is closed by `Tests/UI/ModuleShaderTests.cpp`: `ShaderProgram`
  and `ComputeProgram` expose `graph()`, each renderer whose program is a
  type nested in a `.cpp` has a static `forEachShaderGraph`, and the suite
  emits the GLSL of all 19 shaders the UI, Text, Sprites and GPUWidgets
  modules build and compiles it with glslang on every platform, so a dialect
  regression in a module shader fails on macOS and Windows CI rather than
  waiting for a Vulkan device.
- One Clang-only unity-build fix on the way: `GlyphRenderer.cpp` says `using
  namespace eacp::GPU` at file scope, a unity TU carries that into the
  rasterizer, and a FreeType `pixel_mode` byte compared to an enumerator made
  Clang instantiate the EDSL's constrained `operator==` with a builtin type
  and reject it before the constraint was checked (GCC defers the check).
  Compared as `FT_Pixel_Mode` now, which finds the builtin comparison and
  never looks.

Follow-ups found on the way, not yet done:

- No bidi: a mixed-direction line is shaped run by run in logical order, each
  run in its script's own direction, with no reordering between them. The fix
  is a paragraph-level pass above the rasterizer, not inside it.
- A VS16 sequence is still itemized into two runs: the base codepoint gets
  the colour font and the lone U+FE0F falls out to its own item and its own
  fallback lookup. Harmless for the glyph that matters, untidy.
- No stock Linux family has an `opsz` axis, so the optical-size pin is
  unexercised there; COLRv1 fonts went through `FT_LOAD_COLOR` untested (CBDT
  and COLRv0 were). `lightText` has no FreeType counterpart.
- The `Apps/GPU` example shaders are built on Linux but their GLSL is compiled
  by no test; `ModuleShaderTests` covers the library modules only. Assessed
  under stage 6 below.

**Stage 6 — the follow-ups and the first of the "later" items, landed
2026-09-07** (verified: Linux with `EACP_LINUX_GRAPHICS=ON` 1437 tests under
GCC, headless and again against a real GNOME/Mutter session with
`EACP_REQUIRE_DISPLAY=1`, `EACP_REQUIRE_GPU=1` and `EACP_REQUIRE_FONTS=1`;
option off 572; `GPUTests` 292, `WaylandWindowTests` 13, `TextTests` 103. No
validation layer is installed on that machine and no macOS or Windows compiler
was reached, so the three-line `Texture-Apple.mm`/`Texture-Windows.cpp` guards
below wait on CI.)

- Written as three parallel slices, one per module group, each verified in
  its own build directory and then as one tree.
- Vulkan. `VulkanShared` owns one `VkPipelineCache`, passed to every graphics
  and compute pipeline create, loaded at device creation from
  `$XDG_CACHE_HOME/eacp/pipelines-<pipelineCacheUUID>.bin` (else
  `$HOME/.cache/eacp/`) when the `VkPipelineCacheHeaderVersionOne` names this
  device, written back through a temp file and rename at teardown, every
  failure silent. The §3.1 hash cache above it was looked at and deliberately
  not built: a `RenderPipeline`/`ComputePipeline` is one object and one create
  call and nothing in the backend makes an equal one twice, so a hash cache
  would only dedupe pipelines a caller built twice and would need lifetime
  rules to be safe. `Texture-Linux.cpp` transitions a pixel-less texture into
  its resting layout at creation (`settleAtRestingLayout`, through the copy
  recording uploads use) and refuses a multisampled sampleable-depth target on
  a device without `SAMPLE_ZERO` depth resolve, the query moved out of
  `Frame-Linux.cpp`'s function-local static into
  `VulkanShared::resolvesDepthBySampleZero()`. A negative `bytesPerRow` is
  refused at all four entry points of all three `Texture` backends. The
  `-Wclass-memaccess` warning had already gone in bc2ec73. Tests:
  `TextureCreation/anUnwrittenTextureCanBeRead`,
  `TextureCreation/aNegativeStrideIsRefused`.
- Wayland. The clipboard is a `wl_data_device` on the seat
  (`Window/WaylandClipboard-Linux.{h,cpp}`), reached from `Core` through a
  backend hook — `Clipboard::Backend`, four `std::function`s with no-op
  defaults, `setBackend`/`clearBackend` in `Core/App/Clipboard-Linux.h`, the
  `addLoopSource` precedent — that `WaylandDisplay` installs when the
  connection and seat come up. `copyText` offers `text/plain;charset=utf-8`,
  `text/plain` and `UTF8_STRING`; `copyFiles` a percent-encoded `text/uri-list`;
  `set_selection` takes the last keyboard enter/key serial and returns false
  with no focus; `getText`/`hasText` read the selection offer's mime list, and
  `getText` receives into a pipe while polling the pipe and the display fd
  together under a 2 s bound so a self-paste — our own `send` running while
  we block — works. Two things found on the way: libdecor's GTK plugin makes
  a `wl_data_device` of its own on the connection and Mutter answers only one
  per client, so ours is created before `libdecor_new` and the code says why;
  and `send` writes on a detached thread holding a `shared_ptr` to the
  payload, because a self-paste has both pipe ends on the message thread and a
  payload past the 64 KB pipe buffer deadlocked. A dead connection
  (`dispatch`/`read_events`/`flush`/`roundtrip` failing) runs
  `WaylandDisplay::connectionLost()`: every global dropped, the loop source
  closed, clipboard and input torn down, each window unmapped and its
  surfaces destroyed through the new `WaylandWindowSurface::onConnectionLost`
  so the view-surface `onLost` fires on the existing sync path, and the
  process stays alive headless. `ViewSurface::requestFrameCallback()` now
  commits the subsurface itself once it has content (before the first buffer
  the request still rides the mapping commit, which Mutter requires), and
  `GPUView-Linux.cpp` dropped its cap timer: an early tick re-requests the
  callback and presents nothing. Tests in `WaylandWindowTests-Linux.cpp`:
  three clipboard round trips (text and unicode, 512 KB untruncated,
  `copyFiles` offers no text), each skipping when focus never arrives since
  Weston headless has no seat, and `zLosingTheConnectionTearsTheWindowsDown`,
  which provokes a fatal protocol error (a second `wp_viewport` on one
  surface) and checks `onLost`, the unmapped window and a surfaceless window
  after it — last in the file because it kills the process's connection.
  `Present/maxFpsPacesContinuousMode` measured 15 frames in 1.5 s at
  `setMaxFps(10)`.
- Text and Core. `Timer-Linux.cpp` posts each tick through a `shared_ptr`
  state whose `alive` flag the destructor clears on the main thread, so a tick
  already queued when the timer dies is a no-op: 39 failures in 200 loaded
  runs of `Timer/destructionStopsTicking` before, 0 after. `Timer.mm` fires
  straight from the run loop and `Timer-Windows.cpp` dispatches through a live
  table keyed by id, so neither has the hazard (Windows has a narrower one: a
  `UINT_PTR` reused for a timer created in the same pump turn). Emoji
  presentation is the Unicode 16.0 `Emoji_Presentation` property
  (`Text/UnicodeEmoji.h`, 80 ranges from `emoji-data.txt`, binary search, VS16
  forcing emoji and VS15 now forcing text) instead of the U+1F000–U+1FAFF
  guess; `hb_buffer_set_language` takes `hb_language_get_default()` once.
  `EACP_HAS_CONTEXT` is a PUBLIC compile definition on `eacp-graphics` and the
  `Graphics.h` umbrella gates `TextMetrics`, `EmbeddedView`, `TextInput` and
  `LayerViews` on it — which caught `SVG/SVG.h` including `SVGParser.h` and so
  `SVGBuilder.h` unconditionally, compiling on Linux only because the umbrella
  leaked `LayerViews.h`; gated the same way now. Tests:
  `Text/emojiPresentationProperty`, `Text/variationSelectorsOverridePresentation`
  (portable, table logic) and `Text/emojiPresentationChoosesTheFace` (Linux,
  the rasterizer's `GlyphFormat` per codepoint, self-skipping as the other font
  tests do). `TextTests` 100 → 103.

Assessed and recorded rather than built:

- IME through `zwp_text_input_v3`. Text reaches a view today only as
  `KeyEvent::characters` from `View::keyDown`, and no platform has a
  pre-edit path — there is no `NSTextInputClient` on macOS and no `WM_IME_*`
  on Windows — so this is inventing the contract, not wiring a Linux half of
  one. The minimum honest shape is two virtuals on `View` beside `keyDown`:
  `textComposed(const CompositionEvent&)` with the pre-edit string and its
  cursor/highlight span, and `textInserted(std::string_view)` for a commit,
  with the rule that keys feeding a live composition must not also arrive as
  `keyDown` characters or every keystroke inserts twice (Mutter sends
  `preedit_string`/`commit_string` for ordinary typing once text-input is
  enabled, so that suppression is mandatory). The Wayland half is small and
  sits where the seat lives: `zwp_text_input_manager_v3` is another optional
  global (Mutter offers it, Weston headless does not), the per-seat
  `zwp_text_input_v3` is owned by `WaylandInput` and enabled as keyboard focus
  moves onto a view that wants text, which needs a `View::acceptsTextInput()`
  or a real focus owner (the Linux `View::focus()` is a stub). The protocol
  batches `preedit_string`, `commit_string` and `delete_surrounding_text`
  until `done`, whose serial is echoed in the next `commit`, and the client
  must send `set_cursor_rectangle` in surface coordinates so the candidate
  window lands under the caret — the one piece the View API cannot supply at
  all today. `delete_surrounding_text` needs a view that exposes and mutates
  surrounding text, which none does; a first cut ignores it and says so. With
  no seat on CI, coverage would be the same self-skipping shape as the
  clipboard cases.
- Compile-checking the `Apps/GPU` example shaders. Each example is one
  `Main.cpp` with its shader at file scope beside the `Vertex` its
  `vertexInput` binds; `compile()` is the device-free EDSL walk,
  `ShaderProgram::graph()` exists and `expectGlslCompiles(graph)` takes it, so
  the only obstacle is that the definitions live in an executable's TU. The
  mechanical fix is a `Shaders.h` beside each `Main.cpp` holding the program
  types and the structs they read (Instancing's three panel programs share
  `SpinProgram::emitBody` and move together; Teapot's mesh data stays), an
  `Apps/GPU/ExampleShaders.h` umbrella, and a `Tests/UI/ExampleShaderTests.cpp`
  constructing each program with a `seen` count, one
  `target_include_directories` on the test. The payoff is largest for the
  seven `EACP_HAS_CONTEXT`-gated examples, whose shaders Linux never builds
  today but whose headers have no Context dependency — and which is where the
  emitter arms the module shaders miss live (matrix builders, cube sampling,
  instance-rate inputs, the `Compute`/`ComputeParticles`/`ComputeImage`/
  `PathBench` kernels). The alternative, an `--emit-glsl` flag on every
  example piped through glslang from ctest, moves no source but needs each
  app to start headlessly and still cannot reach the apps Linux does not
  build.

Follow-ups found on the way, not yet done:

- The initial transition costs an acquire and submit per pixel-less texture
  created outside an open recording; a glyph-atlas-style burst pays it per
  texture. It could be deferred to the first real recording.
- lavapipe stores nothing in its pipeline cache, so the disk cache is a
  functional no-op on every CI lane and a warm hit is exercised nowhere; the
  depth-resolve refusal is likewise unreachable on any device seen. Both are
  verified only as round trips.
- The negative-pitch rule is stated once per backend because there is no
  portable `Texture.cpp`; a thin portable layer over the pimpl calls would
  state it once.
- `EACP_HAS_CONTEXT` is silently 0 in a TU that includes `Graphics.h`
  without linking `eacp-graphics`. Nothing in the tree is in that position
  and the failure would be a missing type, not a miscompile; a
  `#ifndef`/`#error` guard is a one-line change for someone who can run the
  Apple and Windows lanes.
- A clipboard paste blocks the message thread for up to 2 s, and a copy
  before the first focused window returns false. A disconnect caused by the
  compositor dying, rather than by a protocol error, is untested — Mutter is
  the session — though the client sees the two identically.
- `Wayland/frameCallbackArrivesAfterACommit` failed once in a full run under
  heavy build load and passed on every retry; its path is unchanged here.
- Still standing from the stage-6 list: portal file dialogs (there is no
  file-dialog API in eacp to implement against yet), IME as assessed above,
  the xcb fallback, a Cairo/Pango `Context`, `VK_EXT_descriptor_heap`, and
  bidi. D6 remains the user's.

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
| `Widgets/TextInput.cpp`, `Layers/*`, `GraphicsContextImpl`, `Font`, `TextMetrics`, `GraphicUtils`, `EmbeddedView`, `ImageConversion` | **removed from the Linux source list** | They pull in the 2D text stack. `TextInput` is client-drawn on every platform anyway; IME is the real gap. `Path` has since gained a geometry-only Linux half (`Path-Linux.cpp`, stage 2). |
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
