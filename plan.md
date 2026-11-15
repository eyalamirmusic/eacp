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

### Phase 3: WebGPU backend and WGSL

`EACP_HAS_GPU` on the web: `GPU/WebGPU/` over Emscripten's `emdawnwebgpu`.
Shaders first by the shortcut — the existing GLSL emitter, glslang to SPIR-V
(`eacp-spirv`), Tint to WGSL — then a native WGSL emitter in
`eacp-gpu-codegen`. The adapter and device are async, so they are acquired
before `main()` (a pre-run step in the generated JS) and handed in; readback
(`Buffer::read`, `copyDrawable`) is async and waits through JSPI where the
app links the nested pump, a callback otherwise. Brings `Apps/GPU` (minus the
Context overlays) and `GPUTests` under headless Chrome.

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
