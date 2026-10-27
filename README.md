# eacp

A cross-platform C++20 framework that abstracts native OS primitives behind a
single, modern API. Write a desktop or mobile app once and have it use the
platform's own windows, views, input, networking and GPU directly: there is no
bundled renderer, no bundled widget toolkit and no VM. That reaches the GPU
too: a shader is a C++ struct, emitted as Metal, HLSL and GLSL from one source,
and the pipeline around it is one API over all three backends.

## Graphics

Windows, views and input are the platform's own — Cocoa, Win32, UIKit,
Android's NativeActivity, and Wayland or X11 on Linux — so an eacp window
looks, moves and resizes like every other window on the desk.

- **Windows** — the usual options: transparent backgrounds, always-on-top,
  click-through, size and aspect-ratio constraints, fullscreen, rounded
  corners, a drawn application icon, an initial position, and mouse lock for
  a first-person camera. The primary display's frame and work area are a call
  away.
- **Views and 2D drawing** — native views with paint and event callbacks. On
  Apple platforms and Windows there is a 2D drawing context over Core Graphics
  and Direct2D, with paths, fonts and text layout, retained shape and text
  layers, and the platform's own text field.
- **Images** — PNG and JPEG decoded and encoded, resized, cropped and warped
  through SIMD kernels; the source of every texture, cursor, tray icon and
  drag image.
- **Input** — keys travel from the focused view up through its parents; the
  wheel and a trackpad pinch reach the view under the pointer; a touch screen
  delivers every finger separately, or the first finger as the mouse so
  widgets written for one work unchanged. Cursors can be drawn from an image,
  and safe-area insets report what a notch or home indicator covers.
- **File drag and drop** — files dragged in from Finder, Explorer or a DAW's
  browser arrive as paths, and files can be dragged out the other way.
- **Game input** — keyboard, mouse and gamepad state polled once a frame for
  a game loop, up to eight controllers, with Xbox and Switch pads supported.
- **Desktop glue** — menus with shortcuts, tray icons, global hot keys, light
  and dark appearance, and a callback on every display refresh.
- **Embedding, both ways** — a view tree of ours inside a window a host owns
  (a plugin editor, an AUv3 view, an X11 window id), or a foreign toolkit's
  editor inside our layout, with the keys it does not want forwarded back to
  the host so a DAW's shortcuts still work.

## GPU

Metal on Apple platforms, Direct3D 12 on Windows and Vulkan on Linux and
Android, behind one API with nothing backend-specific in any header.
[`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md) is the full account.

- **Shaders in C++** — a shader is a struct whose body records a graph of
  typed values; the emitters print it as MSL, HLSL and GLSL. The CPU vertex
  struct _is_ the vertex layout, uniforms and textures are typed members, and
  anything the backends cannot pack identically is a compile error. The EDSL
  has the float, integer, unsigned and boolean families, matrices, swizzles,
  the intrinsics, branches and loops, arrays, 2D, cube and depth textures,
  storage buffers in either stage, and instancing.
- **Views and frames** — a GPU view owns a swapchain and hands over one frame
  per tick or on demand: continuous or one-shot, a frame-rate cap, MSAA,
  depth and stencil, transparency, frames in flight, a readable drawable. A
  frame is one command buffer: passes, compute and copies in order, with no
  fences to write.
- **Passes and pipelines** — pipeline state named in one descriptor (samples,
  depth, blending, culling, winding); draws plain, instanced or indirect from
  a buffer the GPU filled; occlusion queries; viewport and scissor distinct.
- **Render targets** — into a texture, with depth, multisampled and resolved,
  several targets at once, into one mip level, array slice or volume layer;
  mipmaps generated on the GPU; pixels read back from any target or the
  drawable; copies between buffers and textures; and a frame redirected into
  a texture, which is what puts GPU rendering inside a widget tree.
- **Compute** — the same struct shape: atomics, threadgroup memory and
  barriers, group- and SIMD-group-wide reductions, the SIMD-group matrix with
  fp16, bf16, int8 and int4 weights read where they lie, textures a kernel
  writes, dispatches the GPU sizes, dispatches that overlap on an async queue,
  and a timer on every pass.
- **The same kernel on the CPU** — the graph a kernel records also runs with
  no device at all, on the calling thread, allocation-free and realtime-safe,
  so an audio callback or a driverless box runs the very kernel the GPU does.
  One tier up, tensor-level compute over the same buffers compiles to a
  Core ML program for the Apple Neural Engine.

## UI on the GPU

A widget tree is not a tree of native views. A component is a lightweight
element — bounds, children, paint and events — and a whole tree of them lives
inside one GPU view: one swapchain, one display link, one frame a refresh,
whatever the tree holds. [`Lib/eacp/UI/README.md`](Lib/eacp/UI/README.md)
explains how a frame is built.

- **Drawing** — rectangles, rounded rectangles, lines, shadows, paths with
  solid or gradient fills, images, text, and clipping to rectangles or shapes,
  recorded once and replayed through batched shape, mesh, image and glyph
  renderers. A tree that changed nothing records nothing.
- **Widgets** — labels, buttons, checkboxes, text editors, sliders, knobs,
  scroll panels, viewports, lists, combo boxes, tabs and popup menus, a theme,
  keyboard focus with tab order, drag and drop within the tree and file drops
  from outside it, and finger scrolling that coasts when flung.
- **Layers and animation** — a subtree captured into a texture is clipped,
  faded, transformed and composited as one; animations run on the host's
  clock under their own frame-rate cap.
- **GPU components** — custom GPU rendering inside the tree, drawn into a
  texture of its own and composited like any other layer, so a 3D viewport or
  a shader preview sits among the widgets, clipped and scrolled, rather than
  on top of them.
- **Paths** — vector paths rasterized on the GPU by a compute coverage pass
  into a shared atlas, plus a tessellated fill-and-stroke view with gradients.
- **Text** — glyphs rasterized by CoreText, DirectWrite, FreeType with
  HarfBuzz or Android's own text stack into a GPU atlas, with bidirectional
  layout, emoji and variable fonts.
- **Sprites** — runs of textured quads as one instanced draw per texture,
  including NV12 video frames, which is what the camera and video views draw.
- **SVG** — documents drawn as a component or rendered to an image, the same
  on every platform: gradients, clip paths, opacity, symbols, nested
  documents, dashes, text and embedded images.

## Everything else

- **Application lifecycle** — a struct is the app; one view in one window
  needs no struct at all.
- **Command-line tools** — a console runner, an argument parser that binds
  options to your variables and writes the help, and plain stdout lines.
- **Event loop and threads** — the loop, timers, deferred and delayed calls,
  async results, and on Linux a loop a plugin host's own loop can pump.
- **Files** — UTF-8 paths with `/` everywhere, atomic and memory-mapped
  files, a filtered directory walk, a file watcher, and zip archives read and
  written in bounded memory.
- **Processes** — a child with args, env, stdin and captured output, and a
  build runner that reruns one command on demand.
- **Crash reporting** — a handler process launched from the app's own
  executable writes a report with the crashed thread's symbolised stack,
  metadata and attachments when the app faults, then hands it to the app's
  own code; macOS and Windows.
- **Plugins and hot reload** — shared libraries loaded at runtime, and a
  function or object rebuilt on save and swapped into the running app
  ([`Docs/HotReload.md`](Docs/HotReload.md)).
- **Networking** — an HTTP client with redirects, timeouts and resumable
  downloads, an HTTP server, WebSocket client and server, TCP, IPC and RPC,
  and a resource fetched once and revalidated on later runs.
- **WebView** — the system web view with a typed page bridge generated from a
  schema.
- **ML** — tensor graphs written out as a Core ML package and run on the CPU,
  GPU or Neural Engine ([`Lib/eacp/ML/README.md`](Lib/eacp/ML/README.md)).
- **Camera and video** — capture devices and frames, screen capture, video
  encoding, and MP4 decoding behind a player the caller clocks.
- **SIMD and maths** — image hot loops through
  [ESIMD](https://github.com/eyalamirmusic/ESIMD), and vector and matrix
  types packed exactly as the shader types they cross to the GPU as.
- **Interop** — RAII wrappers for Objective-C and Core Foundation objects.

## Supported platforms

The dividing line is drawing. Everything that never touches a screen builds
everywhere, which is what makes eacp usable for a headless service as well as
a GUI. The graphics stack wraps each platform's own compositor instead of
shipping one: Cocoa and Metal, Win32 and D3D12, UIKit, NativeActivity and
Vulkan, and Wayland or X11 with Vulkan.

| | macOS | Windows | iOS | Android | Linux |
| --- | :---: | :---: | :---: | :---: | :---: |
| App core, files, processes, plugins | ✅ | ✅ | ✅ | ✅ | ✅ |
| Networking | ✅ | ✅ | ✅ | ✅ | ✅ |
| Windows, views, input | ✅ | ✅ | ✅ | ✅ | ✅ |
| 2D drawing, fonts, menus, tray | ✅ | ✅ | ✅ | — | — |
| Game input | ✅ | ✅ | ✅ | ✅ | ✅ |
| GPU and the shader EDSL | ✅ | ✅ | ✅ | ✅ | ✅ |
| Compute kernels on the CPU | ✅ | ✅ | ✅ | ✅ | ✅ |
| Text, sprites, UI components, SVG | ✅ | ✅ | ✅ | ✅ | ✅ |
| WebView | ✅ | ✅ | ✅ | — | — |
| Camera | ✅ | ✅ | ✅ | ✅ | — |
| Video | ✅ | ✅ | — | — | — |
| ML through Core ML | ✅ | — | ✅ | — | — |

Where there is no 2D tier, everything drawn through the GPU — the component
tier, its text and images, and SVG — works in full, and Android keeps its own
image codecs. Gamepads come from GameController on Apple platforms and XInput
on Windows; elsewhere game input is the window's own keys and mouse.

Android is a NativeActivity app on Vulkan 1.1 with touch and soft-keyboard
input, the camera behind a runtime permission prompt, and networking over the
platform's own HTTP and TLS stack; the configure writes an Android Studio
project, and [`Apps/Android/README.md`](Apps/Android/README.md) is the guide.
Linux is a real Wayland or X11 window with input, clipboard, fractional
scaling and mouse lock, and can run every window and GPU test with no display
server at all; [`Docs/Linux.md`](Docs/Linux.md) is the full account.

CI builds and tests macOS, Windows x64 and ARM64, and Linux (including the
graphics stack on Mesa's software Vulkan under Weston and Xvfb), and builds
iOS for the simulator and Android. macOS is the most exercised of them.

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

An app that is one view in one window needs no struct at all:

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

More examples live under [`Apps/`](Apps), grouped by the module they exercise.

## Shaders in C++

There is no shader string anywhere in an eacp app. A shader is a struct whose
body records a graph of typed value handles, and the emitters turn that one
graph into Metal Shading Language for macOS and iOS, HLSL for Direct3D 12 on
Windows and GLSL for Vulkan on Linux and Android, compiled to SPIR-V inside
the binary. Vertex inputs are pulled straight out of the CPU vertex struct, so
that struct _is_ the vertex layout, and a CPU vector crosses to the GPU packed
exactly as the shader type it registers as.

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

The view that draws it is as portable as the shader: upload the typed vertex
array, build the pipeline state from a descriptor, and draw, which binds the
pipeline, the vertices, the uniform block and every assigned texture:

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

A compute kernel is the same struct shape, dispatched from a compute pass on a
command buffer — or, with no device at all, through the CPU interpreter over
the same members:

```cpp
struct ToneKernel final : ComputeProgram
{
    ToneKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto phase = toFloat(i) / sampleRate * frequency * twoPi;
        auto wave = sin(phase) + sin(phase * 2.0f) / 2.0f
                    + sin(phase * 3.0f) / 3.0f;
        write(output, i, wave);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> frequency;
    Uniform<Float> sampleRate;
    EACP_SHADER(output, frequency, sampleRate)
};

// On the GPU
auto samples = device.makeBuffer(sizeof(float) * count);
tone.output = samples;
tone.prepare();

auto commands = device.makeCommandBuffer();
{
    auto pass = commands.beginCompute();
    pass.dispatch(tone, count);
}
commands.commit();

// On the CPU, on this thread, allocating nothing
auto wave = std::vector<float>(count);
auto executor = CpuCompute::Executor {tone};
auto bindings = CpuCompute::Bindings {};
bindings.set(tone.output, wave);
executor.dispatch(bindings, count);
```

[`Apps/GPU`](Apps/GPU) has a worked example of every piece, from a rotating
triangle to a teapot, stencil shadows, a cube map, a glyph atlas, particles,
async compute and a first-person maze.

## Building

CMake 3.31+ and a C++20 toolchain. Dependencies are fetched automatically at
configure time.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Every option, the dependencies, the Linux packages and how CI runs are in
[`Docs/Build.md`](Docs/Build.md) and [`Docs/Linux.md`](Docs/Linux.md);
[`Apps/Android/README.md`](Apps/Android/README.md) is the Android guide.

## Repository layout

Each subdirectory of `Lib/eacp` is a self-contained area you can include on its
own — take `Network` without pulling in `GPU`.

```
Lib/eacp/
  Core/       App lifecycle, threading, processes, plugins, files, maths
  Network/    HTTP client and server, WebSocket, TCP, IPC, RPC
  Graphics/   Windows, views, input, menus, tray, 2D drawing
  GPU/        Metal / D3D12 / Vulkan, the shader EDSL and its CPU interpreter
  GPUWidgets/ GPU path rasterization and gradients
  Text/       Glyph rasterization, bidi, and a GPU glyph atlas
  Sprites/    Batched textured-quad renderer
  UI/         The component tier: a whole widget tree in one GPU view
  SVG/        SVG parsing and rendering
  ML/         Tensor graphs compiled and run through Core ML
  WebView/    System web view embedding
  Camera/     Capture devices and frames, plus a view to show them
  Video/      Screen capture, encoding and decoding, plus a view for playback
Apps/         Example applications
Tests/        Unit tests
Docs/         The build system and the Linux backend
ThirdParty/   Vendored libraries (miniz, fmt)
CMake/        Build helpers
```

## Documentation

- [`Docs/Build.md`](Docs/Build.md) — build options, dependencies, the
  capability variables and the CI matrix.
- [`Docs/Linux.md`](Docs/Linux.md) — the Linux backend: Wayland, X11, Vulkan
  and FreeType.
- [`Apps/Android/README.md`](Apps/Android/README.md) — building, running and
  shipping on Android.
- [`Lib/eacp/GPU/README.md`](Lib/eacp/GPU/README.md) — the GPU module and the
  shader EDSL end to end, with [`SAMPLERS.md`](Lib/eacp/GPU/SAMPLERS.md)
  beside it.
- [`Lib/eacp/UI/README.md`](Lib/eacp/UI/README.md) — the component tier and
  GPU rendering inside it.
- [`Lib/eacp/ML/README.md`](Lib/eacp/ML/README.md) — the Core ML tier.

## Built with eacp

- **[PureDOOM](https://github.com/eyalamirmusic/PureDOOM)** — DOOM on eacp's
  application, GPU and input stack, with DOOM's own palette shading.
- **[ShaderToyEACP](https://github.com/eyalamirmusic/ShaderToyEACP)** — two
  hundred Shadertoy shaders as C++ shader structs, a measure of what the EDSL
  is missing.
- **[imgui-eacp](https://github.com/eyalamirmusic/imgui-eacp)** — Dear ImGui
  as an ordinary eacp view, drawn by the GPU module.
- **[WhisperEACP](https://github.com/eyalamirmusic/WhisperEACP)** — Whisper
  as compute kernels on Metal, D3D12 and Vulkan, and again through Core ML.
- **[dhewm3](https://github.com/eyalamirmusic/dhewm3)** — a DOOM 3 source
  port with SDL and OpenGL removed: eacp for the OS and input, and one
  renderer on Metal and Direct3D 12 from shaders written once.
- **[MakeASound](https://github.com/eyalamirmusic/makeasound)** — audio
  devices and MIDI ports behind one API, with its probe, demo and synth apps
  hosted in eacp's GPU, UI and web view stack.
- **[CowTerm](https://github.com/jamierpond/CowTerm)** — a GPU-accelerated
  terminal emulator by Jamie Pond.

## Contributing

eacp is developed in the open. Issues, patches and experiments are all welcome;
the examples under `Apps/` are the fastest way in.

## License

MIT — see [LICENSE](LICENSE).
