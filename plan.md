# Web target: the examples in a browser

Branch `web-target`. The status log of the initial web work (Core, Network,
the loop, the tests) is in git history; `Docs/Web.md` describes the tree.

## Goal

As many `Apps/` examples as possible running **unchanged** in a browser: the
same sources and the same `CMakeLists.txt` gating as every other platform,
with the web reaching each app through the capability variables
(`EACP_HAS_DRAW/GPU/TEXT/CONTEXT/CAPTURE/WEBVIEW/COREML`, `EACP_HAS_THREADS`,
`EACP_HAS_SOCKETS`) as its backends arrive, rather than through a list of its
own.

## Impossible on the web

These stay out for good, gated by capability (or `NOT EMSCRIPTEN` where none
fits):

- **Servers, sockets, IPC** (`EACP_HAS_SOCKETS`): `HttpServer`, `TcpServer`,
  `TcpClient`, `WebSocketDemo` (it runs both ends), `HttpRpcServer`,
  `HttpRpcClient`, `IpcDemo`, `IpcCameraDemo`. A page cannot listen, and the
  IPC demos also launch a second process.
- **Plugins / dlopen**: everything under `Apps/Plugins`.
- **Desktop shell a page does not have**: `TrayApp`, `WindowPosition`,
  borderless/`FancyWindow`-style frames, `EmbeddedViewDemo`, `WebViewDragOut`,
  `UserAgent` (a page cannot set its own), `AppBadge` at best partial.
- **Core ML directly** (`EACP_HAS_COREML`): `Apps/ML`. A WebNN or WebGPU
  runner would be a new backend, not this one.

## Roadmap

### Phase 1: gating cleanup and the loop-only apps (in progress)

- [x] `Apps/CMakeLists.txt`: no early web return; web goes through the same
      capability gates, with `Apps/Web` added beside them.
- [x] `EACP_HAS_SOCKETS` on the socket and IPC apps (`Apps/Network`,
      `IpcDemo`, `IpcCameraDemo`); `Apps/Plugins` never added on the web.
- [x] `Apps/Graphics` split: the component-tier apps on `EACP_HAS_TEXT`
      (plus `SOCKETS`/`CAPTURE` where they need them), only `AlertDemo`,
      `AppBadge` and `WindowPosition` on `EACP_HAS_CONTEXT`. Linux is still
      kept out of the directory (see open items). `MixedViews` gated on
      `EACP_HAS_CAPTURE`.
- [x] App target sets unchanged on macOS, iOS and Android (configured here,
      `cmake --build <dir> --target help` diffed before and after); Windows
      and Linux reasoned through the CMake only.
- [x] `eacp_web_executable(<app> NESTED_PUMP)`: links `eacp-core-nested-pump`
      (JSPI), and the launcher runs under `EACP_WEB_TEST_NODE`.
- [x] `Console`, `ZipDemo` on the web with ctest entries (node and headless).
- [x] `OnlineResourceDemo`, `Http`, `BasicHttp` portable: `NESTED_PUMP` on
      the web, defaulting to `https://httpbingo.org` (CORS, no rate limit at
      their pace) and taking a server as an optional positional argument;
      exit 1 on a failed request. ctest entries (node and headless) against
      `TestServer.mjs`, which now answers the httpbin endpoints they use.
      No native ctest entries: node is not a native test dependency.
- [x] An uncaught C++ exception aborts with its message
      (`CMake/Web/UncaughtException.js`, a `--post-js` on every web
      executable), so a `NESTED_PUMP` `main()` that throws ends a page at once
      instead of hanging; `ThrowFromMainHarness` (node and headless).
- [x] `Docs/Web.md`, `CLAUDE.md` updated.

### Phase 2: Window, View and input on a canvas

`EACP_HAS_DRAW` on the web. A `Window` is a `<canvas>` (one per window, or the
page's own), `View` the Linux-neutral seam the way Android reuses it: per-view
records and sync in a `View-Web.cpp`, a `ViewSurfaceBackend` that hands out
the canvas, pointer/keyboard/wheel DOM events fed into `LinuxInput`'s state
machines (key repeat, click counting, wheel accumulation), `Display` from
`devicePixelRatio` and `resize`/`matchMedia`, `DisplayLink` on
`requestAnimationFrame`. Headless under node.

- [x] `EACP_HAS_DRAW` on for Emscripten (top-level `CMakeLists.txt`); GPU,
      TEXT, CONTEXT and the rest still off. `eacp-graphics` has an
      `EMSCRIPTEN` branch ahead of `UNIX`, linking `Window/Window-Web.js`.
- [x] The seam reused as Android does: `View-Linux.cpp` itself (no
      `View-Web.cpp` was needed), `LinuxWindowState`/`LinuxWindowNative`, the
      headless native, the Linux stubs (menus, tray, hot keys, layers),
      `Path-Linux`, and the stb codec (`Image-Linux.cpp`).
- [x] `Window-Web.cpp`: a window is `Module.canvas` for the first, a canvas of
      its own otherwise; CSS size in points, backing store times
      `devicePixelRatio`; `ResizeObserver` for a layout the page or full screen
      changes; visibility, title, cursor, full screen and pointer lock.
- [x] `ViewSurfaceBackend` handing the whole canvas to one presenting view
      (`WebViewSurface-Web.cpp`), `NativeSurfaceHandle::Kind::Web` (`surface`
      the canvas selector, `window` the id), frames on `requestAnimationFrame`.
      Contract in `Docs/Web.md`, "The surface a presenter gets".
- [x] Input: pointer, wheel, key, focus events dispatched synchronously; the
      click counter, wheel frame and cursor tracker split out of
      `LinuxInput-Linux` into the portable `Window/PointerTrackers.{h,cpp}`
      (Linux unchanged otherwise). Key repeat is the browser's, not `KeyRepeat`,
      and key text is `KeyboardEvent.key`, so the xkb half stays Linux's.
- [x] `Keyboard-Web.cpp` (DOM `code` table, polled state, layout learnt from
      plain presses), `Display-Web.cpp` (screen, viewport, `devicePixelRatio`,
      `matchMedia` scale watch), `DisplayLink-Web.cpp` on
      `AnimationFrame-Web.cpp`, `SystemAppearance-Web.cpp`
      (`prefers-color-scheme`). `EmbeddedView` not built, as on Android.
- [x] Tests: `GraphicsTests` under node (headless; three thread cases return
      early), with `PointerTrackerTests.cpp` (Linux too) and
      `KeyCodeTests-Web.cpp`; `WebWindowHarness` under node (headless) and in
      headless Chrome (a real canvas driven by synthesised DOM events, checked
      at 1x and by hand at 2x). `Apps/Web/HelloInput` for trying it by hand.
- [x] Native unchanged: macOS target set diffed (identical), Graphics tests
      run; on `linux` `GraphicsTests`, `X11WindowTests` (Xvfb) and
      `WaylandWindowTests` (Weston) built and passed with the tracker split.

Open items:

- A presenting view smaller than its window still gets the whole canvas (the
  Android rule). Per-view canvases stacked over the window's would match the
  subsurface model if Phase 3 wants it.
- Touch is the left button with `fromTouch`; no `TouchEvent`s, no pinch
  `Magnify`, no gamepads, image cursors show the arrow (Phase 5).
- A canvas size the page or full screen imposes skips the window's size
  constraint.
- `Lib/eacp/GPU/Vulkan/VulkanSurface-Linux.cpp`'s switch on
  `NativeSurfaceHandle::Kind` now warns (`-Wswitch`) for `Kind::Web` until a
  `case Kind::Web:` is added beside `Kind::Android` (GPU files were Phase 3's).

Not yet verified:

- A `devicePixelRatio` change while running (zoom, a move between displays):
  the `matchMedia` path has not run; 2x was checked only as a launch flag.
- Real (trusted) input in a visible browser: only synthesised events in
  headless Chrome; `HelloInput-run` by hand is untried. Full screen and
  pointer lock, which need a user gesture, are untried.
- A page that sets `Module.canvas`: `Launch.mjs`'s page sets none, so only
  owned canvases have run.
- Firefox and Safari; Windows' clang-cl and MSVC with the tracker split
  (reasoned: the files are Linux/web only).

### Phase 3: WebGPU backend and WGSL

`EACP_HAS_GPU` on the web: `GPU/WebGPU/` over Emscripten's `emdawnwebgpu`,
with a native WGSL emitter in `eacp-gpu-codegen`. Done, bar the open items.

- [x] Shader path decided: a native WGSL emitter (`ShaderBackend::WebGPU`,
      `emitWgsl`), not GLSL → glslang → Tint. The browser takes WGSL only;
      Tint is a large slice of Dawn to build as a host tool or into the wasm,
      and its SPIR-V reader refuses what the GLSL emitter leans on (combined
      image samplers, format-less storage images). The EDSL was modelled on
      WGSL's built-ins, so the dialect sits beside MSL/HLSL/GLSL, device-free
      and checked by `GPUCodegenTests` everywhere (`WgslCodegenTests.cpp`).
      660 of 663 programs emitted by the suites compiled in Chrome; the 3 are
      device limits (read_write RG16Float/RG11B10Float/RG32Sint, 36 KB of
      workgroup memory).
- [x] `EACP_HAS_GPU` on for Emscripten (top-level `CMakeLists.txt`).
- [x] The backend: device, buffers, textures (2D/cube/array/volume, mips,
      block formats, MSAA and depth companions), samplers, compute pipelines
      and dispatch (1D dispatches past 65535 groups fold into rows), render
      pipelines (one variant per depth format; an empty fragment for a
      vertex-only library), off-screen passes and multi-target passes,
      copies, readback.
- [x] Device acquired before `main()`: `CMake/Web/WebGPU.js`, a `--pre-js`
      from `eacp-gpu`, with a run dependency; every adapter feature and its
      highest limits; uncaptured errors printed as `WebGPU: ...`.
- [x] Waiting (readback, `CommandBuffer::wait`, compile messages, pipeline
      validation) suspends through JSPI with the nested pump, and is skipped
      (reads, with a message) or optimistic without it.
- [x] `GPUView` on the canvas (`GPUView-Web.cpp`), over Phase 2's
      `NativeSurfaceHandle::Kind::Web`; continuous mode on
      `requestAnimationFrame`.
- [x] `GPUTests` in headless Chrome, one page per group
      (`eacp_web_discover_tests(GPUTests HEADLESS)`, `Launch.mjs --webgpu`):
      781 of 805 cases pass; every hand-written shader has a WGSL twin.
      `GPUWidgetsTests` under node (device cases stand down).
- [x] `Apps/GPU` without a 2D overlay build and run in a page: `Triangle`
      (a `Triangle.wgsl` twin), `TriangleGen`, `RotatingTriangle`, `Teapot`,
      `ComputeParticles`, `ComputeImage`, `Paths`, `PathCoverage`, `Texture`,
      `Maze`, `Sprites`, `Clipping`, `StreamingStress`, `TextureAtlas` and
      `HotReloadGPU` drew in headless Chrome with no validation error
      (screenshots, by hand); `Compute`, `AsyncCompute` (ctest entries) and
      `PathBench` (by hand) compute and exit 0. `eacp_web_executable(WEBGPU)`.
- [x] Native unchanged: macOS `GPUCodegenTests` 322/322, `CpuComputeTests`
      231/231, `GPUTests` 812/812, `GPUWidgetsTests` 77/77.
- [x] `Docs/Web.md`, `Lib/eacp/GPU/README.md` ("The web"), `CLAUDE.md`,
      `Docs/Build.md` updated.

Open items (the 24 failing `GPUTests` cases, 11 ctest groups, all registered
and red rather than hidden):

- Occlusion queries (9) and GPU timestamps not wired up
  (`supportsOcclusionQueries`/`supportsPassTimings` false).
- Worker-thread `Device` cases (4): the web has no threads.
- No border colour in WebGPU (`Sampler/clampToBorderReadsTheBorderColour`,
  `SamplerBindingDevice/theBoundSamplerDecidesTheAddressMode`).
- Multisampled sampleable depth refused: WebGPU resolves no depth (2).
  A resolve shader would bring it.
- Storage binds off the 256-byte offset grid bind nothing (`RenderRanges`, 2):
  the tests bind at 16.
- RG16Unorm/RGBA16Unorm sample only as unfilterable float, which a layout read
  from WGSL cannot know (`Formats/sixteenBitAndPackedFormatsSample`); a
  per-format pipeline variant, as storage formats have, would fix it.
- A buffer a kernel writes and a dispatch reads indirectly in one pass is one
  WebGPU usage scope (`IndirectDispatch/aGuardedStageStopsAtTheExactCount`).
- `Precompiled` require-flag cases (2): no precompiled shaders on the web;
  `CompilationInfoDevice/everyInvalidLibraryHoldsAnError` fails on the same
  require-flag lines.
- Not done: GPU mip generation (CPU chains only), layered rendering,
  count-driven indirect draws, `copyDrawable` of a canvas only with
  `setReadableDrawable`, a `Buffer::readAsync` for apps without the nested
  pump, `CpuCompute`'s thread on the web, a web build of `Apps/Plugins`'
  GPU demos (plugins are impossible anyway).

Not yet verified:

- WebGPU anywhere but Chrome 157 on macOS's Metal adapter: Linux and Windows
  Chrome, SwiftShader (it comes up headless with
  `--use-webgpu-adapter=swiftshader`), Firefox, Safari.
- The `GPUTests` per-group ctest entries under `ctest -j` load (run at -j6
  here, fine) and the whole-suite page beyond ~20 s.
- A `GPUView` resize, scale change, transparency and MSAA on a real page (the
  examples ran at a fixed size in headless Chrome); `HotReloadGPU` only
  statically.
- Release web builds of the GPU suite.

### Phase 4: Text

`EACP_HAS_TEXT` on the web: FreeType and HarfBuzz from Emscripten's ports,
reusing `GlyphRasterizer-Linux.cpp` with fontconfig replaced by a small
bundled-font resolver (DejaVu/Noto embedded as resources). The Bidi pass
already builds. Brings `Apps/UI`, `Apps/SVG`, most of `Apps/Graphics`
(`GUI`, `TaskBoard`, `Todo`, `KeyInspector`, `CursorShapes`, `MenuBarApp`).

### Phase 5 and later: smaller extras and the rest

- Touch (pointer events), the Gamepad API for `GameInput`, CSS cursors,
  clipboard through the paste event and `navigator.clipboard`, alerts as an
  in-page dialog, async file pickers (`<input type=file>`, download), a menu
  bar stub.
- `WebView` as an `<iframe>` with a `postMessage` bridge for `ScriptHost`.
- A Canvas2D `Context` (`EACP_HAS_CONTEXT`): `AlertDemo`, the `Apps/GPU`
  overlays.
- Camera through `getUserMedia` (`EACP_HAS_CAPTURE`), Video through WebCodecs.
- Persistence (IDBFS/OPFS), opt-in threads, a web CI lane.

## Open items from the initial web work

### Decisions to confirm

- iOS no longer asserts on a process launch attempt in debug builds; the shared
  `Process-Default.cpp` reports `launched()` false.
- `eacp_add_plugin` on the web is a `FATAL_ERROR`.
- Test discovery (`CMake/Web/DiscoverTests.cmake`) is kept.
- The hot-reload sources stay in the web `eacp-core`.
- No automatic JSPI-flag retry for node 24; it must be named as a list.
- A blocking call after `quit()` in a nested pump completes (loop-level fix)
  rather than returning an error.
- `Apps::run` stops the `callAfter` scheduler on every platform.
- Phase 1: the component-tier `Apps/Graphics` apps are still kept off Linux
  (`EACP_HAS_TEXT AND NOT LINUX` in `Apps/CMakeLists.txt`) to keep the Linux
  app set unchanged; they have never been built there.

### Known and left alone

- Web: `TaskGroup::wait()` from a task can still return early when the group's
  remaining task is further up the same stack.
- Native: destroying a group from its own task or callback deadlocks in
  `closeGroup`, as before this work.
- `ThreadPool::stop()` resetting `mainThreadRunPending` and the hosted path
  have no test: reaching the first needs `detachCurrentThreadAsMain`, which
  would break the harness's loop.
- A pending `callAfter` no longer keeps node alive after `main()` returns; no
  permanent test.
- Release web builds print `uncaught exception` without the type and message
  (`getExceptionMessage` is exported only by Debug builds).
- The network examples' defaults depend on httpbingo.org staying up and
  CORS-friendly; their ctest entries do not.

### Not yet verified

- The Windows ARM64 lanes and the iOS and Android CI lanes with this tree.
- Running the clang-cl test binaries; they were built only.
- The Linux display passes (Weston, Xvfb) with the Clang build.
- Firefox and Safari: untried. Only Chrome has run any of this in a browser.
- In a browser, only the app entries ran; the timeouts other than the
  whole-request one, cancel, size limit, resume, zip unpacking and the
  WebSocket failure paths pass under node only.
- Release web builds: the after-exit test patterns print nothing there.
- A node 24 list passed as `EACP_WEB_TEST_NODE`.
- The no-Chrome `STATUS` path; the Chrome search on Windows and Linux hosts;
  `Launch.mjs browser` (`<app>-run`) on Windows and Linux.
- Phase 1 gating on Windows and Linux (reasoned, not configured).
- There is no web CI lane.
