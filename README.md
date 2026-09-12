# eacp

A cross-platform C++20 framework that abstracts native OS primitives behind a
single, modern API. eacp lets you write desktop and mobile applications once
and have them target the platform's first-class primitives directly. The heavy
lifting stays with the OS: there is no bundled renderer, no bundled widget
toolkit and no VM. That reaches the GPU too: a shader is a C++ struct, emitted
as Metal and HLSL from one source, and the pipeline around it is one API over
both backends.

## What it abstracts

eacp wraps the platform's native building blocks rather than reimplementing
them, so apps inherit the look, feel, and performance of the host OS:

- **Application lifecycle** — a templated `Apps::run<T>()` entry point that
  wires up the platform's main event loop.
- **Event loops & threading** — `EventLoop`, `Timer`, `DisplayLink`, and
  `callAsync` on top of CFRunLoop / NSTimer / CADisplayLink (and equivalents
  on Windows).
- **Graphics** — `Window`, `View`, `Path`, `Font`, and a `Context` drawing
  abstraction backed by Core Graphics / CoreText on Apple platforms and the
  native Windows graphics stack. `primaryDisplay()` reports the screen's frame
  and work area in points, so an app can pick a first window size that fits;
  `View::getWindow()` lets a view reach the window it is in rather than be
  handed it.
- **GPU** — `GPUView`, frames, passes, buffers, textures and pipelines over
  Metal and D3D12, plus compute — and a shader EDSL that makes a shader a C++
  struct rather than a string literal per backend. See
  [`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md).
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
  on later runs, and unpacks a zip — `Apps/Console/OnlineResource` fetches
  one before it does anything else. Every fetch reports into the
  `OnlineResources` registry, and `UI::OnlineResourceMonitor` shows that
  registry as a list with progress bars and a Clear all button —
  `Apps/UI/ResourceMonitor` is it in a window over DownloadAndPlay's clips.
- **SVG** — parsing and rendering of SVG documents into the graphics layer.
- **Processes & plugins** — launch a child process with args, env and working
  directory, feed its stdin and capture its output (`eacp::Processes`), and load
  and unload shared libraries at runtime (`DynamicLibrary`).
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
and RPC, the SIMD kernels — builds on Linux too, which is what makes eacp usable
for a headless service as well as for a GUI. The graphics stack builds on all
four platforms, because it wraps each one's own compositor instead of shipping
one: Cocoa and Metal, Win32 and D3D12, UIKit, and Wayland and Vulkan.

| Module | macOS | Windows | iOS | Linux |
| --- | :---: | :---: | :---: | :---: |
| `Core` — lifecycle, event loops, timers, processes, plugins, files | ✅ | ✅ | ✅ | ✅ |
| `Network` — HTTP client and server, WebSocket client, TCP, IPC, RPC | ✅ | ✅ | ✅ | ✅ |
| `SIMD` — portable kernels with runtime backend dispatch | ✅ | ✅ | ✅ | ✅ |
| `Graphics` — windows, views, widgets, menus, drawing | ✅ | ✅ | ✅ | ✅ † |
| `GPU` / `GPUWidgets` — Metal, D3D12, Vulkan and the shader EDSL | ✅ | ✅ | ✅ | ✅ |
| `Text` / `Sprites` — glyph rasterization, atlas, batched quads | ✅ | ✅ | ✅ | ✅ |
| `UI` / `SVG` — component tier and SVG rendering | ✅ | ✅ | ✅ | ✅ † |
| `WebView` — WKWebView and WebView2 | ✅ | ✅ | ✅ | — |
| `Camera` / `CameraView` — capture devices and frames | ✅ | ✅ | ✅ | — |
| `Video` / `VideoView` — screen capture, encode, playback | ✅ | ✅ | — | — |

† Linux has no platform 2D tier and no menus; what that costs is spelled out
two paragraphs down.

Linux graphics is on wherever the graphics modules are built, exactly as the
other three platforms are, and it is three things. A Wayland
`eacp-graphics`: a `Window` is a `wl_surface` with an xdg-shell toplevel
decorated by libdecor, the view tree, hit-testing and input routing are the
portable ones with the seat's pointer and keyboard translated into them through
xkbcommon, a `GPUView` gets a `wl_subsurface` of its own kept at its bounds and
scaled by the compositor's fractional scale, `Display` reports the first output,
mouse lock goes through pointer-constraints, the clipboard is a
`wl_data_device` on the seat (text and `text/uri-list`, installed into
`Core`'s `Clipboard` through a backend hook so `eacp-core` still links no
Wayland), a compositor that goes away mid-session tears every window down
through the same `onLost` path a hidden view takes and leaves the process
running headless, and the display's connection is pumped by eacp's own event
loop. What a window cannot do there is what the protocol has no words for — a
position, a raise, an icon — and the file says so where it matters. A Vulkan backend under it: everything from `Device` to
`RenderPass` is real, the drawable `Frame` renders into a swapchain image and
presents it, and `GPUView` owns that swapchain — mailbox or FIFO, frames in
flight, rebuilt on resize and `OUT_OF_DATE`, with continuous rendering paced by
the compositor's frame callbacks rather than a clock — beside the off-screen
render-and-read-back path every pixel test rides. And a text stack beside them:
`eacp-text`'s glyph rasterizer on FreeType, HarfBuzz and fontconfig, so
`Sprites`, `UI` and the portable half of `SVG` build and run too — a whole
widget tree, its text, its images and its SVG documents drawn inside one
`GPUView` through the coverage rasterizer and the glyph atlas.

What Linux still does not have is the platform's own 2D tier. There is no
`Graphics::Context` and no `Graphics::Font` — `Path` exists, but only as
recorded geometry — so the retained `ShapeLayer`/`TextLayer` and the views over
them, `TextInput`, `EmbeddedView`, the image codecs (an `Image` is a pixel
container there, and loading a file yields an invalid one), menus and the tray
are absent or honest stubs. `SVG`'s native-layer builder and the `SVG::parse`
in front of it go with them; the same document parses and draws through
`SVGComponent`. Under `EACP_HEADLESS=1`, or with no compositor to reach, every
window is built and never shown and every GPU test still runs on Mesa's
lavapipe with no display server at all; the window and present tests run for
real under a headless Weston.

The top-level `CMakeLists.txt` decides this once, in six capability variables
that `Lib`, `Apps` and `Tests` all read rather than restating the platform test.
The three drawing ones are on together on every platform that draws — they
stay three nested variables because each gates a different set of modules, and
a new port reaches them one at a time; the other three hang off
`EACP_HAS_DRAW` and are Apple/Windows-only:

| Variable | On when | Gates |
| --- | --- | --- |
| `EACP_HAS_DRAW` | `EACP_BUILD_GRAPHICS`, and Apple, Windows or Linux | `Graphics`, and `Tests/Graphics` |
| `EACP_HAS_GPU` | `EACP_HAS_DRAW`, and Apple, Windows or Linux | `GPU`, `GPUWidgets`, `Sprites`, their tests and `Apps/GPU` |
| `EACP_HAS_TEXT` | `EACP_HAS_GPU`, and Apple, Windows or Linux | `Text`, `UI`, `SVG`, their tests, `Apps/UI` and the GPU examples that draw glyphs |
| `EACP_HAS_CONTEXT` | `EACP_HAS_DRAW`, and Apple or Windows | the platform's own 2D tier: `Graphics::Context`, `Font`, `TextMetrics`, `TextInput`, `EmbeddedView`, the retained layers and layer views, the image codecs — and so `SVGBuilder`, `Apps/Graphics`, `Apps/Plugins`, `Apps/SVG` and the examples that paint a 2D overlay |
| `EACP_HAS_CAPTURE` | `EACP_HAS_DRAW`, and Apple or Windows | `Camera`, `CameraView`, `Video`, `VideoView` |
| `EACP_HAS_WEBVIEW` | `EACP_HAS_DRAW` and `EACP_BUILD_WEBVIEW`, and Apple or Windows | the native `WebView` (WKWebView / WebView2) |

`EACP_HAS_CONTEXT` is also a compile definition on `eacp-graphics`, so the
`Graphics.h` umbrella leaves the 2D-tier headers out where it is off and a
caller reaching one fails to compile rather than to link.

Two pieces of the gated modules are portable and so sit outside all six: they
are built and tested on every platform, Linux included, because neither touches
a device. `eacp-gpu-codegen` is the shader EDSL and the MSL, HLSL and GLSL
emitters — string generation with no GPU under it, checked by
`GPUCodegenTests`. And `eacp-webview-bridge` is the page bridge over a
`ScriptHost` rather than over a web view, checked by `ScriptHostTests`.

A third, `eacp-spirv`, wraps glslang as a GLSL-to-SPIR-V compiler
(`SpirvTests`) and is built on Linux only by default: the Vulkan backend is the
one that ships it, and the two Linux lanes without a Vulkan device build it
too, so the GLSL dialect is compiled for real there before any device is
involved — every GLSL source `GPUCodegenTests` emits, every hand-written GLSL
twin in `GPUTests` and every module shader `UITests` reaches is compiled by
glslang as part of the test. macOS and Windows skip the fetch; passing
`-DEACP_BUILD_SPIRV=ON` there builds it and turns those checks on. Passing
`OFF` on Linux is only meaningful together with `-DEACP_BUILD_GRAPHICS=OFF`:
the Vulkan backend has no shader compiler in the OS, so a Linux graphics build
without it stops at configure time and says so.

`-DEACP_BUILD_GRAPHICS=OFF` builds the portable half on any platform, Linux
included — it is the only switch that turns the graphics modules off. CI
builds headless and then runs the suite inside a headless Weston session, and
the `Dockerfile` reproduces both:

```bash
docker run --rm -e EACP_HEADLESS=1 -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 \
    -v "$PWD":/workspace eacp-ci-linux \
    ci-build -DEACP_UNITY_BUILD=OFF

docker run --rm -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_DISPLAY=1 \
    -e EACP_REQUIRE_FONTS=1 -v "$PWD":/workspace eacp-ci-linux \
    with-weston ctest --test-dir build-ci-linux --output-on-failure
```

The Vulkan half needs no new build dependency: the headers, `volk` and the
allocator are fetched by CPM, and the loader is opened by name at runtime, so
all a machine needs to run it is a driver — `mesa-vulkan-drivers` is enough, and
its software rasterizer is what CI uses. `EACP_VK_SOFTWARE=1` asks for that
device by preference; `EACP_REQUIRE_GPU=1` turns "no device" from a suite that
silently skips into a suite that fails. The Wayland and text halves are found
the way libcurl is, by pkg-config against the machine's own libraries:
`libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev
libdecor-0-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev pkg-config` on
Debian/Ubuntu, `weston` to run the window tests without a desktop
(`Scripts/with-weston`, which is also `with-weston` in the image), and fonts for
the text tests to resolve — `fonts-dejavu-core fonts-dejavu-extra
fonts-droid-fallback fonts-noto-color-emoji`. `EACP_REQUIRE_DISPLAY=1` does for
the compositor what `EACP_REQUIRE_GPU=1` does for the device, and
`EACP_REQUIRE_FONTS=1` does it for the fonts, which is the third way a suite can
report green by skipping everything.

CI builds every configuration in that matrix and runs the test suite on macOS
(universal), Windows x64 and ARM64 (MSVC and clang-cl) and Linux (GCC, Clang,
and a Clang lane that runs the graphics backend on lavapipe under a headless
Weston — all three build it, one has a device and a compositor to run it on);
iOS is built for the simulator. macOS is the most exercised of them, and Android is not supported.

The HTTP client is one API over three backends — NSURLSession on Apple
platforms, WinHTTP on Windows, libcurl on Linux — so a Linux build needs
libcurl's development headers (`libcurl4-openssl-dev` on Debian/Ubuntu).

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
iOS and into HLSL for Direct3D 12 on Windows. Vertex inputs are pulled straight
out of the CPU vertex struct, so that struct _is_ the vertex layout; uniforms
and textures are typed members assigned by name, and `Maths::Vec2` crosses to
the GPU packed exactly as the `float2` it registers as.

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
is the same struct shape, with atomics, threadgroup memory, barriers and a
dispatch the GPU sized. What the two backends cannot pack the same way — a
`Bool` or a `Float3x3` uniform — is a `static_assert` rather than a footnote.
[`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md) is the full account, and
`Apps/GPU` has a worked example of every piece.

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

### Build options

- `EACP_UNITY_BUILD` (default `OFF`) — compiles eacp libraries as CMake unity
  builds, which is markedly faster for a cold full-project build. It is off by
  default because a unity build collapses per-file entries in
  `compile_commands.json`, which is what language servers read:

  ```bash
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=ON
  ```

- `EACP_BUILD_GRAPHICS` (default `ON`) — builds the whole drawing half. Turn it
  off to build only the portable modules, on any platform.

- `EACP_CI_BUILD` (default `OFF`) — the single switch that reproduces CI's exact
  configuration locally. It forces `EACP_UNITY_BUILD` and `MIRO_UNITY_BUILD` on,
  and turns on `EACP_PCH`, a precompiled header shared across every target that
  is worth roughly half the compile time of a cold Windows build.

  ```bash
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_CI_BUILD=ON
  ```

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
  GPU/        Metal / D3D12: device, buffers, textures, pipelines, passes, and
              the shader EDSL — see GPU/README.md
  GPUWidgets/ Views drawn on the GPU (gradients, paths)
  Text/       Font metrics, glyph rasterization and a GPU glyph atlas
  Sprites/    Batched textured-quad renderer
  UI/         A whole widget tree in one GPUView
  SVG/        SVG parsing and rendering
  WebView/    System web view embedding
  Camera/     Capture devices and frames, plus CameraView/ to show them
  Video/      Screen capture and encoding, plus VideoView/ for playback
Apps/         Example applications
Tests/        Unit tests
ThirdParty/   Vendored single-file libraries (miniz, behind eacp::Zip)
CMake/        Build helpers (TargetSetup, CPM)
```

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

- **[CowTerm](https://github.com/jamierpond/CowTerm)** — a GPU-accelerated
  terminal emulator and session manager by Jamie Pond. Every visible pixel is
  composited on the GPU from a CoreText glyph atlas; sessions, a fuzzy palette
  and persistent pane layouts replace the tmux-sessionizer workflow.

## Contributing

eacp is developed in the open. Issues, patches and experiments are all welcome;
the examples under `Apps/` are the fastest way in.

## License

MIT — see [LICENSE](LICENSE).
