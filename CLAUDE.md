# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Rules

Claude must never commit or push without explicit permission from the user in
the current conversation.

## Project Overview

eacp is a cross-platform GUI/graphics framework written in modern C++20 with Objective-C++ interop. It provides abstractions for application lifecycle, graphics rendering, threading, GPU, and networking.

Platform coverage splits on whether a module draws, decided once in the
top-level `CMakeLists.txt` by six capability variables that `Lib`, `Apps` and
`Tests` read instead of restating the platform test: `EACP_HAS_DRAW`
(`Graphics` and `Tests/Graphics`), `EACP_HAS_GPU` (`GPU`, `GPUWidgets`,
`Sprites`, `Apps/GPU`), `EACP_HAS_TEXT` (`Text`, `UI`, `SVG`, `Apps/UI` and the
GPU examples that draw glyphs), `EACP_HAS_CONTEXT` (the platform's own 2D tier —
`Graphics::Context`, `Font`, `TextMetrics`, `TextInput`, `EmbeddedView`, the
retained `ShapeLayer`/`TextLayer` and their views, the image codecs — and so
`SVGBuilder`, `Apps/Graphics`, `Apps/Plugins`, `Apps/SVG`, `Apps/UI/SVGDocument`
and the GPU examples that paint a 2D overlay), `EACP_HAS_CAPTURE` (`Camera`,
`CameraView`, `Video`, `VideoView`, the last two additionally off on iOS) and
`EACP_HAS_WEBVIEW` (the native `WebView`). The first three are
`APPLE OR WIN32 OR LINUX` — everywhere graphics builds at all — and stay
nested (`TEXT` implies `GPU` implies `DRAW`) because each gates a different
set of modules and a new port reaches them one at a time. The other three hang off
`EACP_HAS_DRAW` and are Apple/Windows-only, so on those two platforms all six
are simply what `EACP_HAS_DRAW` alone used to decide. `Core`,
`Network` and `SIMD` build everywhere, Linux included, and so do two device-free
pieces of the gated modules: `eacp-gpu-codegen`, the shader EDSL and the
MSL/HLSL/GLSL emitters (`GPUCodegenTests`), and `eacp-webview-bridge`, the page
bridge over a `ScriptHost` (`ScriptHostTests`). `eacp-spirv` (`GPU/Spirv/`)
wraps glslang as a GLSL-to-SPIR-V compiler (`SpirvTests`); it is built on
Linux only by default (`EACP_BUILD_SPIRV`), because only the Vulkan backend
ships it, and macOS and Windows can opt in. Where it is built, every GLSL
source the codegen tests emit — and every hand-written GLSL twin in `GPUTests` —
is compiled by glslang inside the suite, so an emitter regression fails on every
Linux CI lane, the two with no Vulkan device included, rather than only as a
wrong pixel on the lane that has a device. See the table in `README.md`. CI
builds and tests macOS, Windows (x64 and ARM64, MSVC and clang-cl) and Linux
(GCC, Clang, and a Clang lane that runs the Vulkan backend on Mesa's lavapipe,
with the tests inside a headless Weston session so windows and swapchains are
real; all three Linux lanes build the whole graphics stack and install the
stock font packages so the text suites resolve rather than skip, and only the
third has a driver and a compositor to run the GPU and window tests on), and
builds iOS for the simulator.

Dependencies are fetched by CPM at configure time — `ea_data_structures`, `Miro`,
`ResEmbed` and, behind `EACP_BUILD_SPIRV` and so on Linux only by default,
`glslang`; a Linux build adds
`Vulkan-Headers`, `volk` and `VulkanMemoryAllocator` (`CMake/FindVulkanBackend.cmake`,
one `eacp-vulkan` target, fetched on no other platform). Plus libcurl on Linux,
which backs the HTTP client there, and — on Linux, where they are not
optional — two pkg-config groups: the Wayland client library,
`wayland-protocols` with `wayland-scanner`, xkbcommon and libdecor (`CMake/FindWayland.cmake`, one
`eacp-wayland` target holding the generated protocol code), and FreeType,
HarfBuzz and fontconfig for the glyph rasterizer (`CMake/FindLinuxText.cmake`,
one `eacp-linux-text` target). Nothing links `libvulkan`: `volkInitialize()`
opens it by name at runtime, so a machine with no driver builds the same binary
and reports `Device::isValid()` false.

One dependency is carried in the tree instead: `ThirdParty/miniz`, the
amalgamated miniz 3.1.2 pair beside its MIT license, built as its own C target
so it never joins a unity build and its warnings are silenced. Only `eacp-core`
links it, PRIVATE, and only `Utils/Zip.cpp` includes its header, so the whole
of it is reached through `eacp::Zip`. To update it, replace the files under
`ThirdParty/miniz` and the version in its README.

## Build Commands

```bash
# Configure
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF

# Build all targets
cmake --build build

# Build specific target
cmake --build build --target GUI
cmake --build build --target Console
```

Output executables:
- `build/Apps/GUI/GUI.app` (macOS bundle)
- `build/Apps/Console/Console` (command-line app)

### Build Options

- `EACP_UNITY_BUILD` (default `OFF`): compiles eacp libraries as CMake unity
  builds for faster full-project compilation. It is off by default precisely
  because a unity build collapses the per-file entries in
  `compile_commands.json` that LSP tooling reads; Claude must keep it off, and
  pass `-DEACP_UNITY_BUILD=OFF` explicitly so a cached `ON` in an existing build
  directory does not survive a reconfigure.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
```

- `EACP_CI_BUILD` (default `OFF`): the single switch CI passes to reproduce the
  exact CI configuration locally. It force-enables the unity-build flag of every
  project that exposes one — `EACP_UNITY_BUILD` and `MIRO_UNITY_BUILD` — and
  turns on `EACP_PCH`. Because it turns unity on, it is for reproducing CI, not
  for LSP-backed development — Claude should keep using
  `-DEACP_UNITY_BUILD=OFF` for normal work.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_CI_BUILD=ON
```

- `EACP_PCH` (default `OFF`, on under `EACP_CI_BUILD`): shares one precompiled
  header — the STL — across every eacp target, which is worth roughly a third of
  the compile time of a typical translation unit. It holds no eacp header on
  purpose, but editing `CMake/Pch.h` still rebuilds the project, so it is off
  for normal work and on in CI, where every build is cold anyway.

  `<windows.h>` is deliberately **not** in it. CMake builds a PCH with `/FI`, so
  the payload is force-included into every translation unit, and windows.h
  brings two dozen macros with it — `near` and `far` among them, which are
  lowercase and so collide with ordinary member names. Measured: it saves a
  portable TU nothing (475ms against an STL-only image, 478ms with windows.h
  added, 692ms with no image), and costs the `*-Windows.cpp` TUs that do want it
  a flat ~68ms each to parse it through `WinInclude.h` instead.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DEACP_PCH=ON
```

- `EACP_VERBOSE_CONFIGURE` (default `OFF`): prints the configure detail a clean
  configure leaves out — the FetchContent population of the `DOWNLOAD_ONLY`
  Vulkan sources, the pkg-config and `find_package` probes and the `check_*`
  results under them. CPM's own line per package is not part of that and prints
  either way: it names the source, the tag, and any local override, which is
  the one thing worth reading. `CMake/ConfigureLog.cmake` is the whole of it:
  it raises `CMAKE_MESSAGE_LOG_LEVEL` to `VERBOSE` and drops the `QUIET` it
  otherwise hands the finders. Warnings and errors sit above the log level, so
  nothing this hides is something that went wrong.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DEACP_VERBOSE_CONFIGURE=ON
```

- `EACP_BUILD_SPIRV` (default `ON` on Linux, `OFF` elsewhere): builds
  `eacp-spirv`, fetching glslang via CPM (a shallow ~75 MB checkout, about 5 s
  of build on a laptop, a minute on a 4-core CI runner). It is on where
  something ships it — the Vulkan backend has no shader compiler in the OS —
  and off on macOS and Windows, whose backends compile their own dialects, so
  those builds skip the fetch. Passing `ON` there builds the compiler and turns
  the GLSL compile checks in `GPUCodegenTests`, `GPUTests` and `UITests` back
  on, which is how to check the emitter locally on a Mac. Consumers test
  `if (TARGET eacp-spirv)`.

- `EACP_WEBVIEW_DEV` (default `OFF`): skips the Vite production build and
  resource embedding for webview apps. The UI is served from the Vite dev
  server instead (`npm run dev` in the app's `web/` dir); the runtime already
  prefers a reachable dev server (`Options::Embedded::preferDevServer`).
  Schema codegen still emits TS into `web/src/generated` on every app build.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DEACP_WEBVIEW_DEV=ON
```

### Local Miro source

Miro is fetched via CPM from `eyalamirmusic/Miro` by default. To work against a
local Miro checkout (e.g. while co-developing both repos), pass
`-DCPM_Miro_SOURCE=$HOME/Code/Miro` at configure time. CPM honours
`CPM_<Name>_SOURCE` automatically and uses the local path instead of the GitHub
fetch.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DCPM_Miro_SOURCE=$HOME/Code/Miro
```

Use `$HOME` (not `~`). CMake does not expand `~`, and shell tilde expansion is
suppressed inside quotes — `-DCPM_Miro_SOURCE="~/Code/Miro"` will silently
configure against a non-existent path and fail later with errors like
`Unknown CMake command "miro_add_type_export"`.

## The Linux Backend

Linux draws through Wayland, Vulkan and FreeType, and it is on wherever the
graphics modules are — `EACP_HAS_DRAW`, `EACP_HAS_GPU` and `EACP_HAS_TEXT` are
true there exactly as they are on Apple and Windows. `EACP_HAS_CONTEXT` is not:
there is no 2D backend.

`eacp-graphics` is one process-wide Wayland connection
(`Window/WaylandDisplay-Linux.cpp`: registry, outputs, libdecor context, the
surface-to-window map, and the loop source that pumps it through
`Threads::addLoopSource` with a pre-poll flush), a `Window` that is a
`wl_surface` under a libdecor frame with a viewport-stretched shm buffer behind
the content (`Window-Linux.cpp`), the portable view tree with a `wl_subsurface`
for every view that asks for one (`View-Linux.cpp` implementing the
`ViewSurface` contract in `View-Linux.h`), seat input translated through
xkbcommon into the portable hit-tester with pointer-constraints for mouse lock
(`Window/WaylandInput-Linux.cpp`), the clipboard as a `wl_data_device` on the
seat (`Window/WaylandClipboard-Linux.cpp`, installed into `Core`'s `Clipboard`
through the backend hook in `Core/App/Clipboard-Linux.h` so `eacp-core` links
no Wayland; a copy needs keyboard focus on one of our windows), a compositor
disconnect that fires `onLost` on every view surface and leaves the process
headless, the evdev-to-`KeyCode` table (`Graphics/Keyboard-Linux.h`), `Display`
from the first output, a `DisplayLink` paced at the output's refresh rate, and
stubs for image codecs, menus, tray and system appearance.

Under it is the Vulkan backend (`GPU/Vulkan/`): everything from `Device` to
`RenderPass` is real, the drawable `Frame` presents a swapchain image, and
`GPUView-Linux.cpp` owns the swapchain over the view's subsurface
(`VK_KHR_wayland_surface`; mailbox or FIFO; frames in flight on the context
timeline; rebuilt on resize and `OUT_OF_DATE`; continuous mode paced by
`wl_surface.frame` callbacks, with `setMaxFps` skipping early ticks rather than
running a timer), every pipeline built through one `VkPipelineCache` persisted
under `$XDG_CACHE_HOME/eacp/`, with the off-screen `renderNativeContent` path
unchanged beside it. The GPU module knows Wayland as two opaque pointers and
neither links nor includes it. Under `EACP_HEADLESS=1` or with no
`WAYLAND_DISPLAY` to reach, a window is built with no surface, exactly the
headless backend this grew out of. Device loss is terminal (no `VkDevice`
rebuild; `onDeviceRestored` never fires).

The text half is `Text/GlyphRasterizer-Linux.cpp` on FreeType, HarfBuzz and
fontconfig, found by pkg-config through `CMake/FindLinuxText.cmake` into one
`eacp-linux-text` target that `eacp-text` links PRIVATE, so nothing above the
module sees a FreeType header. Above it sits the one piece of `eacp-text` that
is portable but only Linux calls: `Text/Bidi.{h,cpp}` and `Text/UnicodeBidi.h`,
the Unicode Bidirectional Algorithm (UAX #9) over tables generated from the
Unicode 16.0 database, run as a paragraph pass before itemization so a mixed
Hebrew/Arabic/Latin line shapes in visual order. It is built on all three
platforms and called on none of the others: CTLine and IDWriteTextLayout
reorder inside themselves, so `GlyphRasterizer::shape()` returns visually
ordered glyphs everywhere and nothing reorders twice. All of it makes
`eacp-text` real on Linux and with it `eacp-ui`, the portable half of
`eacp-svg`, `Apps/UI` (minus `SVGDocument`) and `Apps/GPU`'s `GlyphAtlas` and
`VariableFont`. It needs font files as well as libraries — a font test asks
fontconfig for a family and self-skips when nothing resolves, so an
installation with no fonts runs the Text suite as a silent green;
`EACP_REQUIRE_FONTS=1` turns that skip into a failure, and the packages CI and
the `Dockerfile` install for it are `libfreetype-dev libharfbuzz-dev
libfontconfig-dev fonts-dejavu-core fonts-dejavu-extra fonts-droid-fallback
fonts-noto-color-emoji`.

What stays absent is `EACP_HAS_CONTEXT`: no 2D `Context`, so `Font`,
`TextMetrics`, `TextInput`, `EmbeddedView`, the retained layer classes and the
image codecs are left out of the Linux source list rather than stubbed, and
with them `SVGBuilder`/`SVG::parse`, `Apps/Graphics`, `Apps/Plugins`,
`Apps/SVG`, `Apps/UI/SVGDocument` and the `Apps/GPU` examples that paint a 2D
overlay. `EACP_HAS_CONTEXT` is also a PUBLIC compile definition on
`eacp-graphics`, and the `Graphics.h` umbrella leaves those headers out
where it is 0. `Path` is there as recorded geometry only
(`Primitives/Path-Linux.h`). Every GPU test but the Metal-only
`TextureInteropTests.mm` runs there on lavapipe.

`-DEACP_BUILD_GRAPHICS=OFF` is the only way to build Linux without any of this;
there is no Linux-specific switch. The `Dockerfile` reproduces both halves of
the CI Linux lanes:

```bash
docker run --rm -e EACP_HEADLESS=1 -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 \
      -e EACP_REQUIRE_FONTS=1 -v "$PWD":/workspace eacp-ci-linux \
      ci-build -DEACP_UNITY_BUILD=OFF

docker run --rm -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_DISPLAY=1 \
      -e EACP_REQUIRE_FONTS=1 -v "$PWD":/workspace eacp-ci-linux \
      with-weston ctest --test-dir build-ci-linux --output-on-failure
```

`EACP_VK_SOFTWARE=1` prefers a CPU device (Mesa's lavapipe), mirroring
`EACP_D3D12_WARP`; `EACP_REQUIRE_GPU=1` makes `GPUTests` fail rather than
self-skip when no device came up; `EACP_VK_VALIDATION=1` turns on
`VK_LAYER_KHRONOS_validation` with a debug-utils messenger that logs. The
second command is how the window and present tests (`WaylandWindowTests`,
`GPUTests`' `Present` cases) run for real: `Scripts/with-weston` (also
`with-weston` in the image) wraps a command in a headless Weston session,
and `EACP_REQUIRE_DISPLAY=1` makes those tests fail rather than self-skip
without a compositor, as `EACP_REQUIRE_FONTS=1` does for the font tests.
Weston's headless backend has no seat, so input is never exercised there.
See `Lib/eacp/GPU/README.md`.

## Architecture
New source files are added directly to the module's CMakeLists.txt under the
appropriate `target_sources(...)` call. Platform-specific sources go inside the
matching `APPLE`/`IOS`/`WIN32`/`LINUX` branch.

### Core Library (`Lib/eacp/Core`)

**App/** - Application lifecycle management
- `App<T>`: Template wrapper for user-defined app structs
- `run<T>(args...)`: Template function that starts the event loop; `args` are
  copied and handed to T's constructor on every construction, restarts included
- Entry point pattern: define a struct and pass to `eacp::Apps::run<MyApp>()`.
  A struct that is one view in one window is `Graphics::ViewWindow<MyView>`
  (`Graphics/Window/ViewWindow.h`), and `Graphics::runWindowedApp<MyView>(
  options, viewArgs...)` runs one as the app with no struct written at all; a
  struct that does more still pairs its view and window in one member,
  `Window window {view, options};`

**Graphics/** - Rendering and UI
- `Context`: Abstract base for drawing operations; `MacOSContext` is the Core Graphics implementation
- `View`: UI component base class with `paint(Context&)` and `mouseDown(MouseEvent)` virtual methods
- `Window`: macOS window wrapper with configurable flags
- `Path`: Vector path drawing (rect, ellipse, curves)
- `Font`: CoreText-based typography
- `Primitives.h`: Basic types (`Point`, `Rect`, `Color`)

**Threads/** - Event loop and timing
- `EventLoop`: CFRunLoop wrapper with `run()`, `quit()`, `call(Callback)`
- `callAsync(Callback)`: Schedule function on main thread
- `callAfter(Time::MS, Callback)`: the same, delayed. One shared scheduler
  thread serves every pending callback, so N deadlines cost one thread, not N;
  `Threads::delay` is built on it (`Threads/CallAfter.cpp`)
- `Timer`: periodic callbacks, taking either `Time::MS` or an integer Hz
- `DisplayLink`: CADisplayLink-backed V-sync synchronized callbacks
- `addLoopSource(fd, events, cb)` / `removeLoopSource(fd)`
  (`Threads/EventLoop-Linux.h`, Linux only): a pollable descriptor joining the
  loop's own `poll()` set, so a Wayland or xcb connection can be pumped by
  eacp's loop without `eacp-core` linking the library that owns it; the
  four-argument overload adds a `prepare` callback run before every `poll()`,
  which is where the Wayland connection flushes its requests and dispatches
  events another reader left queued

**Network/** - HTTP and WebSocket abstraction
- `Request`/`Response` structs with `httpRequest()` function (NSURLSession backed)
- Multipart parts come from a path (`addFileField`) or from bytes already in
  memory (`addFileBytes`/`FileField::fromBytes`, no temporary file needed)
- `urlEncode`/`urlDecode` and `parseQueryString` (`HTTP/Http.h`)
- `OnlineResource` (`Network/OnlineResource/`): a file an app needs from the
  network, kept under `FilePath::appSupportDirectory() / "Resources"` and
  fetched at most once. A sidecar (`<path>.resource.json`) records the URL,
  the app-declared version and the server's ETag / Last-Modified; a later
  fetch re-downloads on a URL or version change, revalidates with one
  conditional GET when the server gave validators, and otherwise trusts the
  copy. A `.zip` is unpacked and `path()` is the folder. Three tiers: the
  stateful object (`start()` returning `Threads::Async<Result>`, `cancel()`,
  `progress()` readable from any thread), `fetchAsync(Options)` with the
  callbacks in `Options`, and the blocking `fetch(Options)` for a console app
  that needs the file before its loop runs. Destroying the object abandons
  its Async, so a dead view is never called back. `Apps/Console/OnlineResource`
  and `Apps/Video/DownloadAndPlay` are the two users.
- `OnlineResources` (`Network/OnlineResource/OnlineResources.h`): the
  process-wide registry every `OnlineResource` reports into as it starts,
  finishes and removes, keyed by the path the file lands at. It owns the
  app's one resource directory (`setDirectory`/`getDirectory`, which is
  what `OnlineResource::defaultDirectory()` answers). An `Entry` is
  whether a complete copy is on disk and how big, what last happened
  (`Status`: idle, fetching, fetched, failed, cancelled) with the error, and
  the live `Progress` of a transfer in flight. `declare()` lists a resource
  in the directory before anything fetches it, `fetch(path)` runs one the
  registry owns, `cancel`/`remove`/`forget` act on one entry, and `clear()`
  deletes the directory — refused while anything under it is fetching.
  Listeners are called on the main thread once per loop turn after a state
  change; progress is polled. `UI::OnlineResourceMonitor` (`UI/Network/`,
  its own `eacp-ui-network` target so `eacp-ui` stays free of
  `eacp-network`) is the registry as a list with a progress bar per
  transfer and Fetch / Cancel / Delete copy / Clear all buttons.
  `OnlineResourceMonitorHost` is it as a whole component tree and
  `OnlineResourceMonitorWindow` is that host in a window of its own, so an
  app sets the directory, declares its resources and constructs one;
  `Apps/UI/ResourceMonitor` does exactly that over DownloadAndPlay's own
  folder.
- `WebSocket::Connection` (`Network/WebSocket/`): a client over the same three
  platform stacks - Network.framework's `nw_ws` (`WebSocket.mm`;
  NSURLSessionWebSocketTask's cancelWithCloseCode: drops its close frame on
  GitHub's macOS runners), WinHTTP's WebSocket API,
  libcurl's `curl_ws_*` (`isSupported()` is false where libcurl lacks it, as on
  Ubuntu 24.04's 8.5.0). `WebSocket.cpp` is the one state machine, marshalling
  every `Sink` report to the message thread through `Threads::callAsync`; each
  `WebSocket-<Platform>` file implements `Backend.h`'s `makeBackend` and
  nothing else. `Protocol.h` is RFC 6455 framing, spoken by
  `WebSocket::Server` (`Server.h`: over `TCP::Listener`, an accept thread and
  one per client, clients addressed by `ClientId`, callbacks on the message
  thread like the client's) and by the tests' misbehaving server.
  `Apps/Network/WebSocketDemo` runs both ends in one process. The library is
  one translation unit under a unity build, so every file-scope name in
  `WebSocket/` is prefixed `webSocket`/`WebSocket`.

**Process/** - Child process launch and control (`eacp::Processes`)
- `Process`: launch an executable with args/env/working dir; captures stdout and
  stderr, feeds stdin, and exposes `wait()`/`isRunning()`/`terminate()`/`kill()`
- `run()`: blocking convenience returning a `ProcessResult`; `runAsync()` returns
  a `Threads::Async<ProcessResult>` resolved on the main thread
- POSIX impl (`Process-Posix.cpp`, fork/exec) shared by macOS+Linux; Windows uses
  `CreateProcessW` (`Process-Windows.cpp`)

**ObjC/** - Memory management bridge
- `Ptr<T>`: RAII smart pointer for Objective-C objects (handles retain/release)
- `CFRef<T>`: RAII wrapper for Core Foundation types
- `AutoReleasePool`: RAII wrapper for NSAutoreleasePool

**Utils/** - Generic patterns
- `FilePath::appSupportDirectory()` / `appCacheDirectory()`: this app's own
  folder under the per-user data and cache roots, `<root>/<Company>/<App>`.
  The names come from the embedded `AppInfo.json` (`Platform::getAppName`,
  `getCompanyName`; the company is the target's `EACP_COMPANY_NAME` property
  or the variable of that name, and its level is omitted when empty); an app
  with no `AppInfo` is named after its executable (`Files::executablePath`).
  The two-argument overloads take the names instead
- `Pimpl<T>`: Pointer-to-implementation pattern
- `Singleton<T>::get()`: Thread-safe singleton
- `Vectors`: Container algorithms (`contains`, `eraseMatch`, `find`)
- `Base64::encode`/`decode`: RFC 4648, the framework's only implementation -
  the WebSocket handshake's accept key goes through it too
- `Zip::Reader`/`Zip::Writer` (`Utils/Zip.h`): zip archives over the vendored
  miniz, read from a file (memory-mapped) or from bytes, written to either.
  `extractAll` refuses entries that would land outside the target directory.
  `Zip::compress`/`decompress` deflate a single blob as a zlib stream. It is
  compiled with `MINIZ_NO_STDIO`: every file goes through `MemoryMappedFile`
  and `Files::writeFile`, so UTF-8 paths take the same route as everything
  else. `Apps/Console/Zip` is the worked example.

### Key Design Patterns

- **Pimpl**: Platform-specific implementations hidden behind abstract interfaces
- **Template Factory**: `run<T>()` creates applications from user-defined structs
- **RAII**: Automatic resource cleanup via C++ destructors, especially for ObjC/CF objects
- **View Hierarchy**: Composable UI through `addSubview()`/`removeSubview()`

### Framework Dependencies

macOS: Foundation, Cocoa, CoreVideo, CoreGraphics, CoreText, Metal.
Windows: Direct2D, DirectWrite, D3D11/D3D12, DXGI, DirectComposition, WinHTTP.
Linux: pthreads, libcurl, wayland-client, wayland-cursor, xkbcommon, libdecor,
FreeType, HarfBuzz and fontconfig, plus the Vulkan loader, opened with `dlopen`
rather than linked.

## Code Style

Always use the most modern C++ and RAII practices.
Use auto for variables and whenever possible.
Don't use auto for functions and member functions

Don't use comments unless absolutely needed. Use named functions to make code self documenting.

Give std::function members a non-null default — a no-op lambda, or one
returning an empty value (e.g. `[] { return Image {}; }`) — so call sites
invoke them directly without null checks.


Enforced via `.clang-format`:
- Allman brace style
- 85 column limit
- 4-space indentation (no tabs)
- Pointer alignment: left (`int* ptr`)
- Break constructor initializers before comma

Always run clang-format for edited code files