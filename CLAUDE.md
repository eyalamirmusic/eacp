# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Rules

Claude must never commit or push without explicit permission from the user in
the current conversation.

## Project Overview

eacp is a cross-platform GUI/graphics framework written in modern C++20 with Objective-C++ interop. It provides abstractions for application lifecycle, graphics rendering, threading, GPU, and networking.

Platform coverage splits on whether a module draws, decided once in the
top-level `CMakeLists.txt` by five capability variables that `Lib`, `Apps` and
`Tests` read instead of restating the platform test: `EACP_HAS_DRAW`
(`Graphics` and `Tests/Graphics`), `EACP_HAS_GPU` (`GPU`, `GPUWidgets`,
`Sprites`, `Apps/GPU`), `EACP_HAS_TEXT` (`Text`, `UI`, `SVG` and the examples
that draw text), `EACP_HAS_CAPTURE` (`Camera`, `CameraView`, `Video`,
`VideoView`, the last two additionally off on iOS) and `EACP_HAS_WEBVIEW` (the
native `WebView`). The first three are nested — `TEXT` implies `GPU` implies
`DRAW` — because Linux reaches them one plan stage at a time; Apple and Windows
have all three, so their target sets are what `EACP_HAS_DRAW` alone used to
decide. `Core`,
`Network` and `SIMD` build everywhere, Linux included, and so do two device-free
pieces of the gated modules: `eacp-gpu-codegen`, the shader EDSL and the
MSL/HLSL/GLSL emitters (`GPUCodegenTests`), and `eacp-webview-bridge`, the page
bridge over a `ScriptHost` (`ScriptHostTests`). `eacp-spirv` (`GPU/Spirv/`)
wraps glslang as a portable GLSL-to-SPIR-V compiler so the GLSL dialect can be
compiled on every platform (`SpirvTests`); only the Vulkan backend and the tests
link it, never a shipping macOS/Windows binary. Where it is built, every GLSL
source the codegen tests emit — and every hand-written GLSL twin in `GPUTests` —
is compiled by glslang inside the suite, so an emitter regression fails on macOS
and Windows CI rather than waiting for a Vulkan device. See the table in `README.md`. CI
builds and tests macOS, Windows (x64 and ARM64, MSVC and clang-cl) and Linux
(GCC, Clang, and a Clang lane with `EACP_LINUX_GRAPHICS=ON` running the Vulkan
backend on Mesa's lavapipe), and builds iOS for the simulator.

Dependencies are fetched by CPM at configure time — `ea_data_structures`, `Miro`,
`ResEmbed` and, behind `EACP_BUILD_SPIRV`, `glslang`; a Linux graphics build adds
`Vulkan-Headers`, `volk` and `VulkanMemoryAllocator` (`CMake/FindVulkanBackend.cmake`,
one `eacp-vulkan` target, fetched on no other platform). Plus libcurl on Linux,
which backs the HTTP client there. Nothing links `libvulkan`: `volkInitialize()`
opens it by name at runtime, so a machine with no driver builds the same binary
and reports `Device::isValid()` false.

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

- `EACP_BUILD_SPIRV` (default `ON`): builds `eacp-spirv`, fetching glslang via
  CPM (a shallow ~75 MB checkout, about 5 s of build on a laptop, a minute on
  a 4-core CI runner). Off skips the fetch and the target; consumers test
  `if (TARGET eacp-spirv)`.

- `EACP_LINUX_GRAPHICS` (default `OFF`): turns `EACP_HAS_DRAW` and
  `EACP_HAS_GPU` on for Linux; `EACP_HAS_TEXT` stays off there until the
  FreeType backend lands. What it builds today is a headless `eacp-graphics` —
  the portable view tree with `View-Linux.cpp` under it, windows with no surface
  (`Window-Linux.cpp`), a timer-paced `DisplayLink`, and stubs for the display,
  image codecs, menus, tray, keyboard state and system appearance — plus the
  compute half of the Vulkan backend under it (`GPU/Vulkan/`): `Device`,
  `Buffer`, `ShaderLibrary`, `ComputePipeline`, `ComputePass`, `CommandBuffer`
  and `GpuTimestamps` are real, and `Texture`, `RenderPipeline`, `RenderPass`,
  `Frame` and `GPUView` are placeholders that report themselves invalid until
  the render half lands. No 2D `Context`, so `Font`, `TextMetrics`,
  `TextInput`, `EmbeddedView` and the retained layer classes are left out of the
  Linux source list rather than stubbed; `Path` is there as recorded geometry
  only (`Primitives/Path-Linux.h`). `GraphicsTests` runs 118 cases there,
  `GPUTests` 21 and `GPUWidgetsTests` 53.

```bash
docker run --rm -e EACP_HEADLESS=1 -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 \
      -v "$PWD":/workspace eacp-ci-linux \
      ci-build -DEACP_LINUX_GRAPHICS=ON -DEACP_UNITY_BUILD=OFF
```

  `EACP_VK_SOFTWARE=1` prefers a CPU device (Mesa's lavapipe), mirroring
  `EACP_D3D12_WARP`; `EACP_REQUIRE_GPU=1` makes `GPUTests` fail rather than
  self-skip when no device came up; `EACP_VK_VALIDATION=1` turns on
  `VK_LAYER_KHRONOS_validation` with a debug-utils messenger that logs. See
  `Lib/eacp/GPU/README.md`.

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

## Architecture
New source files are added directly to the module's CMakeLists.txt under the
appropriate `target_sources(...)` call. Platform-specific sources go inside the
matching `APPLE`/`IOS`/`WIN32` branch.

### Core Library (`Lib/eacp/Core`)

**App/** - Application lifecycle management
- `App<T>`: Template wrapper for user-defined app structs
- `run<T>()`: Template function that starts the event loop
- Entry point pattern: define a struct and pass to `eacp::Apps::run<MyApp>()`

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
  eacp's loop without `eacp-core` linking the library that owns it

**Network/** - HTTP and WebSocket abstraction
- `Request`/`Response` structs with `httpRequest()` function (NSURLSession backed)
- Multipart parts come from a path (`addFileField`) or from bytes already in
  memory (`addFileBytes`/`FileField::fromBytes`, no temporary file needed)
- `urlEncode`/`urlDecode` and `parseQueryString` (`HTTP/Http.h`)
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
- `Pimpl<T>`: Pointer-to-implementation pattern
- `Singleton<T>::get()`: Thread-safe singleton
- `Vectors`: Container algorithms (`contains`, `eraseMatch`, `find`)
- `Base64::encode`/`decode`: RFC 4648, the framework's only implementation -
  the WebSocket handshake's accept key goes through it too

### Key Design Patterns

- **Pimpl**: Platform-specific implementations hidden behind abstract interfaces
- **Template Factory**: `run<T>()` creates applications from user-defined structs
- **RAII**: Automatic resource cleanup via C++ destructors, especially for ObjC/CF objects
- **View Hierarchy**: Composable UI through `addSubview()`/`removeSubview()`

### Framework Dependencies

macOS: Foundation, Cocoa, CoreVideo, CoreGraphics, CoreText, Metal.
Windows: Direct2D, DirectWrite, D3D11/D3D12, DXGI, DirectComposition, WinHTTP.
Linux: pthreads, libcurl, and — behind `EACP_LINUX_GRAPHICS` — the Vulkan
loader, opened with `dlopen` rather than linked.

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