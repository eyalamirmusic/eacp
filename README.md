# eacp

A cross-platform C++20 framework that abstracts native OS primitives behind a
single, modern API. eacp lets you write desktop and mobile applications once
and have them target the platform's first-class primitives directly. The heavy
lifting stays with the OS: there is no bundled renderer, no bundled widget
toolkit and no VM. That reaches the GPU too: a shader is a C++ struct, emitted
as Metal, HLSL and GLSL from one source, and the pipeline around it is one API
over all three backends.

## What it abstracts

eacp wraps the platform's native building blocks rather than reimplementing
them, so apps inherit the look, feel, and performance of the host OS:

- **Application lifecycle** — a templated `Apps::run<T>()` entry point that
  wires up the platform's main event loop.
- **Event loops & threading** — `EventLoop`, `Timer`, `DisplayLink`, and
  `callAsync` on top of CFRunLoop / NSTimer / CADisplayLink, their Windows
  equivalents, and on Linux one `epoll` descriptor a plugin host's own loop
  can pump (`getEventLoopFd`, `pumpEventLoop`).
- **Files** — `FilePath` carries a path as UTF-8 text, `Files::readFile` /
  `writeFile` / `writeFileAtomically` move whole files, `File` and
  `MemoryMappedFile` read big ones in pieces, and `Zip` reads and writes
  archives. A directory is walked with `Files::forEachEntry` (a visitor that
  can prune a subtree or stop early, with unreadable entries reported to a
  callback) or collected with `listDirectory` / `listFiles`, recursively or
  not, hidden entries and symlinks by choice.
- **Graphics** — `Window`, `View`, `Path`, `Font`, and a `Context` drawing
  abstraction backed by Core Graphics / CoreText on Apple platforms and the
  native Windows graphics stack. `primaryDisplay()` reports the screen's frame
  and work area in points, so an app can pick a first window size that fits;
  `View::getWindow()` lets a view reach the window it is in rather than be
  handed it. On a touch screen a view that calls `setHandlesTouchEvents()`
  gets every finger as its own `TouchEvent` (`touchBegan` / `touchMoved` /
  `touchEnded`, one id per finger, held by the view it came down on), and any
  other view gets the first finger as the mouse, so widgets written for a mouse
  work unchanged. `View::getSafeAreaInsets()` is what the status bar, a notch or
  the home indicator covers, with `safeAreaInsetsChanged()` when it moves; both
  stay zero and silent on desktop windows.
- **Game input** — `GameInput` is keyboard and mouse state for a game loop,
  polled once a frame beside the `View` callbacks that UI and text entry keep
  using: `snapshot()` returns a frame that answers `isDown`, `wasPressed`,
  `wasReleased` and `mouseDelta`, with every event timestamped on the clock
  `FrameTime` uses. On Apple platforms the events come from the GameController
  framework off the main thread, so they are captured while the app is still
  drawing; elsewhere they come from the window's own events. Input counts
  only while the window has key focus, and losing it releases every key.
  `Apps/GPU/Maze` is a first-person maze driven by it.
- **GPU** — `GPUView`, frames, passes, buffers, textures and pipelines over
  Metal and D3D12, plus compute — and a shader EDSL that makes a shader a C++
  struct rather than a string literal per backend. The same compute kernel also
  runs on the CPU, on the calling thread and without allocating, where no
  device came up or an audio callback needs it now. See
  [`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md).
- **ML** — tensor-level compute as a `Graph` of whole-tensor ops (`linear`,
  `matmul`, `softmax`, `layerNorm`, `conv`, `gather`, attention with a causal
  or run-time mask, `argmax`, and `apply` for anything elementwise, written
  over the shader EDSL's own `Float`), written out as an `.mlpackage` that
  Core ML compiles and runs on the CPU, the GPU or the Apple Neural Engine.
  `Model` caches the compile, predicts blocking or as a `Threads::Async`,
  reports where each op was placed, and its `MultiArray` copies to and from a
  `GPU::Buffer`. The graph builder and writers build everywhere; only the
  runner is Apple. See [`Lib/eacp/ML/README.md`](Lib/eacp/ML/README.md).
- **Widgets & menus** — native text inputs, menus, and embedded views.
- **WebView** — embed a system web view (WKWebView on Apple, WebView2 on
  Windows) with support for popups and new-window requests.
- **Networking** — an `HTTP::Request` / `HTTP::Response` API plus an
  `HTTPServer`, a `WebSocket::Connection` client and `WebSocket::Server`, TCP
  sockets, IPC channels and an RPC layer over both — `Apps/Network/WebSocketDemo`
  runs both WebSocket ends in one process. Backed by NSURLSession and
  Network.framework on Apple platforms, WinHTTP on Windows and libcurl on
  Linux. `OnlineResource` fetches a file an app needs into its own
  Application Support folder once, revalidates it against the server's ETag
  on later runs, and unpacks a zip; `UI::OnlineResourceMonitor` shows every
  such fetch as a list with progress bars. `Apps/Console/OnlineResource` and
  `Apps/UI/ResourceMonitor` are the two examples.
- **SVG** — parsing and rendering of SVG documents into the graphics layer.
- **Processes & plugins** — launch a child process with args, env and working
  directory, feed its stdin and capture its output (`eacp::Processes`), and load
  and unload shared libraries at runtime (`DynamicLibrary`). Both directions of
  plugin UI are covered: `EmbeddedView` puts a view tree of ours inside a window
  a host owns, and `NativeChildSurface` is the inverse, a `View` in one of our
  layouts that hands a foreign toolkit an `NSView*` or a child `HWND` to parent
  its own editor into — what a plugin host passes to `IPlugView::attached()`.
  `EmbedderKeyForwarder` and `KeyGrab` carry the keys a hosted editor does not
  want back to the host's main window, so a DAW's shortcuts still work.
- **Text & sprites** — font metrics, glyph rasterization and a GPU glyph atlas,
  alongside a batched textured-quad renderer for everything that draws in bulk.
- **UI** — a lightweight component tier: a whole widget tree in one `GPUView`,
  drawn through the sprite and glyph batchers.
- **SIMD** — portable kernels with runtime backend dispatch, so one source picks
  the widest instruction set the machine actually has.
- **Maths** — `Vec2` / `Vec3` / `Vec4` and a column-major `Mat4` with the
  transform and projection builders, packed exactly as the shader types they
  register as, so the same value does the CPU-side geometry and crosses to the
  GPU as a vertex field or uniform without repacking.
- **Camera & video** — capture devices and frames with a `CameraView` to show
  them, screen capture, video encoding, and decoded playback through the GPU
  display stack.
- **Interop helpers** — RAII wrappers (`Ptr<T>`, `CFRef<T>`,
  `AutoReleasePool`) for safe Objective-C / Core Foundation interop, plus
  generic utilities (`Pimpl`, `Singleton`, vector helpers).

## Supported platforms

The dividing line is drawing. Everything that never touches a screen — the app
and threading core, processes, plugins, files, the HTTP client and server, IPC
and RPC, the SIMD kernels, the CPU compute interpreter, the ML graph builder —
builds on Linux too, which is what makes eacp usable for a headless service as
well as for a GUI. The graphics stack builds on all four platforms, because it
wraps each one's own compositor instead of shipping one: Cocoa and Metal,
Win32 and D3D12, UIKit, and Wayland or X11 with Vulkan.

| Module | macOS | Windows | iOS | Linux |
| --- | :---: | :---: | :---: | :---: |
| `Core` — lifecycle, event loops, timers, processes, plugins, files | ✅ | ✅ | ✅ | ✅ |
| `Network` — HTTP client and server, WebSocket client, TCP, IPC, RPC | ✅ | ✅ | ✅ | ✅ |
| `SIMD` — portable kernels with runtime backend dispatch | ✅ | ✅ | ✅ | ✅ |
| `Graphics` — windows, views, widgets, menus, drawing | ✅ | ✅ | ✅ | ✅ † |
| `GameInput` — polled keyboard and mouse for a game loop | ✅ ‡ | ✅ | ✅ ‡ | ✅ |
| `GPU` / `GPUWidgets` — Metal, D3D12, Vulkan and the shader EDSL | ✅ | ✅ | ✅ | ✅ |
| `CpuCompute` — the same compute kernels run on the CPU, no device needed | ✅ | ✅ | ✅ | ✅ |
| `Text` / `Sprites` — glyph rasterization, atlas, batched quads | ✅ | ✅ | ✅ | ✅ |
| `UI` / `SVG` — component tier and SVG rendering | ✅ | ✅ | ✅ | ✅ † |
| `WebView` — WKWebView and WebView2 | ✅ | ✅ | ✅ | — |
| `Camera` / `CameraView` — capture devices and frames | ✅ | ✅ | ✅ | — |
| `Video` / `VideoView` — screen capture, encode, playback | ✅ | ✅ | — | — |
| `ML` — tensor graphs compiled and run through Core ML | ✅ | — | ✅ | — |

† Linux has no platform 2D tier: no `Graphics::Context`, `Font`, `TextInput`,
image codecs, menus or tray. Everything drawn through a `GPUView` — the `UI`
component tier, its text, its images and `SVGComponent` — works there in
full, and a `Window` is a real Wayland or X11 window with input, clipboard,
fractional scaling and mouse lock. Audio-plugin hosting has what it needs
(`EmbeddedView` into a host's X11 window, and an event loop a host's own loop
can pump), and `EACP_HEADLESS=1` runs every window and every GPU test with no
display server at all. [`Docs/Linux.md`](Docs/Linux.md) is the full account.

‡ Fed by the GameController framework on Apple platforms, which times events
off the main thread; fed by the window's own key and mouse events elsewhere.

CI builds and tests macOS (universal), Windows x64 and ARM64 (MSVC and
clang-cl) and Linux (GCC, Clang, and a lane that runs the graphics stack on
Mesa's software Vulkan under a headless Weston and then an Xvfb), and builds
iOS for the simulator and Android (API 33+, Vulkan 1.3; the HelloGPU example
for arm64-v8a with the pinned NDK). macOS is the most exercised of them.
Android builds through an Android Studio project the configure writes;
[`Apps/Android/README.md`](Apps/Android/README.md) is the guide.

## A taste of the API

A minimal console app with a recurring timer:

```cpp
#include <eacp/Core/Core.h>

using namespace eacp;

struct App
{
    void update()
    {
        LOG(numTimes);

        numTimes++;

        if (numTimes == 4)
            Apps::quit();
    }

    int numTimes = 0;
    Threads::Timer timer {[&] { update(); }, 1};
};

int main()
{
    return Apps::run<App>();
}
```

A GUI app embedding a web view:

```cpp
#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

WindowOptions windowOptions()
{
    auto options = WindowOptions {};
    options.title = "Browser";
    options.width = 1100;
    options.height = 760;
    return options;
}

// A Window built with its view adopts it as its content, so the pair is two
// members and the constructor body is left for what the app actually does.
struct MyApp
{
    MyApp() { webView.loadURL("https://example.com"); }

    WebView webView;
    Window window {webView, windowOptions()};
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
```

An app that is one view in one window and nothing else needs no struct at all:
`Graphics::runWindowedApp<MyView>(options, viewArgs...)` runs a
`ViewWindow<MyView>` — the view built from `viewArgs`, then the window showing
it — as the app.

```cpp
#include <eacp/Graphics/Graphics.h>

using namespace eacp;

struct HelloView final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        g.setColor(Graphics::Color::white());
        g.fillRect(getLocalBounds());
    }
};

int main()
{
    auto options = Graphics::WindowOptions {};
    options.title = "Hello";

    return Graphics::runWindowedApp<HelloView>(options);
}
```

An HTTP request:

```cpp
#include <eacp/Network/HTTP/Http.h>

auto req = eacp::HTTP::Request::post("https://api.example.com/posts", body);
req.headers["Content-Type"] = "application/json";
auto res = req.perform();
```

More examples live under [`Apps/`](Apps), grouped by the module they exercise:
`Console`, `Network`, `Graphics`, `GPU`, `UI`, `SVG`, `WebView`, `Camera`,
`Video`, `Plugins` and `Mixed`.

## Shaders in C++

There is no shader string anywhere in an eacp app. A shader is a struct that
derives from `ShaderProgram`; `define()` records a graph of typed value handles,
and the emitters turn that one graph into Metal Shading Language for macOS and
iOS, into HLSL for Direct3D 12 on Windows and into GLSL for Vulkan on Linux,
where glslang compiles it to SPIR-V inside the binary. Vertex inputs are
pulled straight out of the CPU vertex struct, so that struct _is_ the vertex
layout; uniforms and textures are typed members assigned by name, and
`Maths::Vec2` crosses to the GPU packed exactly as the `float2` it registers
as.

```cpp
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

struct Vertex
{
    Maths::Vec2 position;
    Maths::Vec2 uv;
};

struct Waves final : ShaderProgram
{
    Waves() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = varying(vertexInput(&Vertex::uv));

        auto ripple = 0.5f + 0.5f * sin(uv.x() * 8.f + time);

        setPosition(float4(position, 0.f, 1.f));
        auto texel = sample(image, uv);
        setFragment(texel * float4(ripple, uv.y(), 1.f, 1.f));
    }

    Uniform<Float> time;
    Uniform<Texture2D> image;

    EACP_SHADER(time, image)
};
```

The view that draws it is as portable as the shader. `setVertices` uploads the
typed vertex array, `prepare` takes a `RenderPipelineDescriptor` — sample
count, depth test and write, blend equation, cull mode, winding — and builds
the pipeline state from it, and `pass.draw(shader)` binds the pipeline, the
vertices, the uniform block and every assigned texture:

```cpp
struct WavesView final : GPUView
{
    WavesView()
        : image(loadTexture())
    {
        shader.setVertices(quad);
        shader.prepare({.sampleCount = sampleCount(),
                        .blendMode = BlendMode::AlphaBlend});
        shader.image = image;
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        shader.time = elapsed;

        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    Texture image;
    Waves shader;
    float elapsed = 0.f;
};
```

Render targets, depth and multisampling, and compute passes all sit on the same
`Frame`, ordered for you, with no fences to write.

The EDSL covers the `Float`, `Int`, `UInt` and `Bool` families and the
matrices, every swizzle, the intrinsic set spelled the way MSL and HLSL spell
it, `var` / `select` / `ifThen` / `loop`, 2D, cube and depth textures, storage
buffers readable from either stage, instancing, and compute — `ComputeProgram`
is the same struct shape, with atomics, threadgroup memory, barriers, SIMD-group
matrix fragments, packed fp16, bf16, int8 and int4 weight reads and a dispatch
the GPU sized. What the backends cannot pack the same way — a `Bool` or a
`Float3x3` uniform — is a `static_assert` rather than a footnote.
[`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md) is the full account, and
`Apps/GPU` has a worked example of every piece.

The graph a kernel records is also what runs it where there is no device.
The CPU compute interpreter runs a `ComputeProgram` on the calling thread over
plain arrays, allocation-free and realtime-safe, so an audio callback or a
driverless Linux box runs the very same kernel the GPU does, and every emitter
test has a numeric half that needs no driver — `Apps/GPU/CpuCompute` times the
two against each other. One tier up, `Lib/eacp/ML` takes tensor-level compute
written against the same buffers and compiles it to a Core ML program, so a
net that runs through the compute kernels on every platform also runs on the
Apple Neural Engine where there is one (see
[`Lib/eacp/ML/README.md`](Lib/eacp/ML/README.md)).

## Building

eacp uses CMake (3.31+) and a C++20 toolchain. Dependencies are fetched via
[CPM](https://github.com/cpm-cmake/CPM.cmake) automatically at configure time,
except [miniz](https://github.com/richgel999/miniz), which is carried in
`ThirdParty/` and wrapped by `eacp::Zip`.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Build a specific example:

```bash
cmake --build build --target GUI       # build/Apps/GUI/GUI.app
cmake --build build --target Console   # build/Apps/Console/Console
```

On Linux the window systems, the text stack and the HTTP client are found by
pkg-config against the machine's own libraries, and the Vulkan loader is
opened at runtime, so a driver is all the GPU half needs:

```bash
sudo apt install pkg-config libcurl4-openssl-dev \
    libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev libdecor-0-dev \
    libxcb1-dev libxcb-xkb-dev libxkbcommon-x11-dev libxcb-randr0-dev \
    libxcb-xfixes0-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-xinput-dev \
    libfreetype-dev libharfbuzz-dev libfontconfig-dev mesa-vulkan-drivers
```

The options most builds reach for:

- `EACP_UNITY_BUILD` (default `OFF`) — unity builds, markedly faster for a cold
  full build, off by default because they collapse the per-file entries in
  `compile_commands.json` that language servers read.
- `EACP_BUILD_GRAPHICS` (default `ON`) — turn it off to build only the
  portable modules, on any platform.
- `EACP_BUILD_SPIRV` (default `ON` on Linux, `OFF` elsewhere) — the GLSL
  compiler the Vulkan backend ships. Passing `ON` on a Mac or a Windows
  machine turns the GLSL compile checks in the GPU test suites on.

Every option, the CPM and pkg-config dependencies, the capability variables
that decide which modules a platform gets, and how CI runs are in
[`Docs/Build.md`](Docs/Build.md).

## Repository layout

Each subdirectory of `Lib/eacp` is a self-contained area you can include on its
own — take `Network` without pulling in `GPU`.

```
Lib/eacp/
  Core/       App lifecycle, threading, processes, plugins, files, vector maths,
              ObjC/CF interop
  Network/    HTTP client and server, WebSocket client, TCP, IPC, RPC
  SIMD/       Portable SIMD kernels with runtime backend dispatch
  Graphics/   Windows, views, widgets, menus, drawing primitives
  GPU/        Metal / D3D12 / Vulkan: device, buffers, textures, pipelines,
              passes, the shader EDSL and its CPU interpreter — see GPU/README.md
  GPUWidgets/ Views drawn on the GPU (gradients, paths)
  ML/         Tensor graphs compiled and run through Core ML — see ML/README.md
  Text/       Font metrics, glyph rasterization and a GPU glyph atlas
  Sprites/    Batched textured-quad renderer
  UI/         A whole widget tree in one GPUView
  SVG/        SVG parsing and rendering
  WebView/    System web view embedding
  Camera/     Capture devices and frames, plus CameraView/ to show them
  Video/      Screen capture and encoding, plus VideoView/ for playback
Apps/         Example applications
Tests/        Unit tests
Docs/         In-depth documentation: the build system and the Linux backend
ThirdParty/   Vendored single-file libraries (miniz, behind eacp::Zip)
CMake/        Build helpers (TargetSetup, CPM)
```

## Documentation

- [`Docs/Build.md`](Docs/Build.md) — every build option, the dependencies,
  the capability variables and the CI matrix.
- [`Docs/Linux.md`](Docs/Linux.md) — the Linux backend: Wayland, X11, Vulkan
  and FreeType, what is and is not there, and the environment variables that
  drive it.
- [`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md) — the GPU module and the
  shader EDSL end to end: pipelines, render targets, compute, packed weights,
  the SIMD-group matrix, running a kernel on the CPU, and the Vulkan backend.
  [`SAMPLERS.md`](Lib/eacp/GPU/SAMPLERS.md) beside it is the sampler model.
- [`Lib/eacp/ML/README.md`](Lib/eacp/ML/README.md) — the Core ML tier: building
  a tensor graph, running a model, the compile cache and what was measured.
- [`Apps/GPU/VariableFont/README.md`](Apps/GPU/VariableFont/README.md) — the
  variable-font example.

## Built with eacp

Four projects lean on different parts of the framework, and between them are
what keeps it honest — each one is a demand the API has to meet, and the gaps
they surface are what gets fixed next.

- **[PureDOOM](https://github.com/eyalamirmusic/PureDOOM)** — DOOM on eacp's
  application, GPU and input stack. The level is drawn as real hardware 3D at
  the window's resolution rather than at 320×200, but the shading is DOOM's own:
  the texture yields a palette index, the `COLORMAP` row picked by sector light
  and distance remaps it, and the palette resolves the colour. Three attract-mode
  demos, 11,410 tics, replay as a test that hashes the world after every tic.

- **[ShaderToyEACP](https://github.com/eyalamirmusic/ShaderToyEACP)** —
  Shadertoy's GLSL turned into eacp GPU programs, shaders authored as C++ structs
  and compiled to Metal and HLSL from one source. A corpus of two hundred real
  fragment shaders turns "what is the shader EDSL missing?" into a measurement:
  every shader that fails to convert names a gap, and the number blocked on each
  gap decides which one to close next.

- **[imgui-eacp](https://github.com/eyalamirmusic/imgui-eacp)** — Dear ImGui as
  an ordinary eacp `View`, drawn by the GPU module, with ImGui's shader written
  once in the EDSL and emitted as both MSL and HLSL. Because it is a real view it
  composes: `Apps/MixedViews` puts it beside a `WebView` in one window, wired in
  both directions.

- **[WhisperEACP](https://github.com/eyalamirmusic/WhisperEACP)** — Whisper
  on eacp's compute stack: the encoder and decoder as EDSL compute kernels over
  packed fp16 weights, running on Metal, D3D12 and Vulkan from one source, and
  the encoder again through `Lib/eacp/ML` on Core ML. It is what drove the
  SIMD-group matrix, the packed weight reads and the Core ML backend, and its
  tests are what say the two ways of running the same net agree.

- **[CowTerm](https://github.com/jamierpond/CowTerm)** — a GPU-accelerated
  terminal emulator and session manager by Jamie Pond. Every visible pixel is
  composited on the GPU from a CoreText glyph atlas; sessions, a fuzzy palette
  and persistent pane layouts replace the tmux-sessionizer workflow.

## Contributing

eacp is developed in the open. Issues, patches and experiments are all welcome;
the examples under `Apps/` are the fastest way in.

## License

MIT — see [LICENSE](LICENSE).
