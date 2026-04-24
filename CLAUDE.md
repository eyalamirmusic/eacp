# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

eacp is a macOS-focused GUI/graphics framework written in modern C++20 with Objective-C++ interop. It provides abstractions for application lifecycle, graphics rendering (Core Graphics), threading (CFRunLoop), and networking. The framework is self-contained with no third-party dependencies beyond macOS system frameworks.

## Build Commands

```bash
# Configure
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug

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

- `EACP_UNITY_BUILD` (default `ON`): compiles eacp libraries as CMake unity
  builds for faster full-project compilation. Claude must always configure with
  `-DEACP_UNITY_BUILD=OFF` so per-file compile commands land in
  `compile_commands.json` and LSP tooling returns accurate results.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
```

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

**Network/** - HTTP abstraction
- `Request`/`Response` structs with `httpRequest()` function (NSURLSession backed)

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

Foundation, Cocoa, CoreVideo, CoreGraphics, CoreText (all macOS system frameworks)

## Code Style

Always use the most modern C++ and RAII practices.
Use auto for variables and whenever possible.
Don't use auto for functions and member functions

Don't use comments unless absolutely needed. Use named functions to make code self documenting.


Enforced via `.clang-format`:
- Allman brace style
- 85 column limit
- 4-space indentation (no tabs)
- Pointer alignment: left (`int* ptr`)
- Break constructor initializers before comma

Always run clang-format for edited code files