# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Rules

Claude must never commit or push without explicit permission from the user in
the current conversation.

## Project Overview

eacp is a cross-platform GUI/graphics framework written in modern C++20 with Objective-C++ interop. It provides abstractions for application lifecycle, graphics rendering, threading, GPU, and networking.

Platform coverage splits on whether a module draws. `Core`, `Network` and `SIMD`
build everywhere, Linux included; the graphics stack (`Graphics`, `GPU`, `Text`,
`Sprites`, `UI`, `SVG`, `WebView`, `Camera`, `Video`) is gated behind
`(APPLE OR WIN32) AND EACP_BUILD_GRAPHICS` in `Lib/eacp/CMakeLists.txt`, with
`Video`/`VideoView` additionally off on iOS. See the table in `README.md`. CI
builds and tests macOS, Windows (x64 and ARM64, MSVC and clang-cl) and Linux
(GCC and Clang), and builds iOS for the simulator.

Dependencies are fetched by CPM at configure time — `ea_data_structures`, `Miro`
and `ResEmbed` — plus libcurl on Linux, which backs the HTTP client there.

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
- `Timer`: NSTimer-backed periodic callbacks
- `DisplayLink`: CADisplayLink-backed V-sync synchronized callbacks

**Network/** - HTTP and WebSocket abstraction
- `Request`/`Response` structs with `httpRequest()` function (NSURLSession backed)
- `WebSocket::Connection` (`Network/WebSocket/`): a client over the same three
  platform stacks - `NSURLSessionWebSocketTask`, WinHTTP's WebSocket API,
  libcurl's `curl_ws_*` (`isSupported()` is false where libcurl lacks it, as on
  Ubuntu 24.04's 8.5.0). `WebSocket.cpp` is the one state machine, marshalling
  every `Sink` report to the message thread through `Threads::callAsync`; each
  `WebSocket-<Platform>` file implements `Backend.h`'s `makeBackend` and
  nothing else. `Protocol.h` is RFC 6455 framing, spoken by the tests' server.
  The library is one translation unit under a unity build, so every file-scope
  name in `WebSocket/` is prefixed `webSocket`/`WebSocket`.

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

### Key Design Patterns

- **Pimpl**: Platform-specific implementations hidden behind abstract interfaces
- **Template Factory**: `run<T>()` creates applications from user-defined structs
- **RAII**: Automatic resource cleanup via C++ destructors, especially for ObjC/CF objects
- **View Hierarchy**: Composable UI through `addSubview()`/`removeSubview()`

### Framework Dependencies

macOS: Foundation, Cocoa, CoreVideo, CoreGraphics, CoreText, Metal.
Windows: Direct2D, DirectWrite, D3D11/D3D12, DXGI, DirectComposition, WinHTTP.
Linux: pthreads and libcurl.

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