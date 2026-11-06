# Getting started with eacp

eacp is a cross-platform C++20 framework over the platform's own windows,
views, input, networking and GPU (macOS, iOS, Windows, Linux, Android). There is
no bundled renderer or widget toolkit: a window is a Cocoa, Win32, UIKit,
Wayland/X11 or NativeActivity window, and the GPU is Metal, Direct3D 12 or
Vulkan behind one API. Apps are a struct handed to `eacp::Apps::run<T>()`.

## Contents

- [Consuming it with CMake](#consuming-it-with-cmake)
- [Platform matrix](#platform-matrix)
- [A window with a custom View](#a-window-with-a-custom-view-apple-and-windows)
- [UI components](#the-same-on-every-platform-ui-components)
- [GPUComponent](#gpu-drawing-inside-the-tree-gpucomponent)
- [Text in a GPUComponent](#text-in-a-gpucomponent)
- [Timer](#timer-every-100-ms-then-stop-it)
- [HTTP and JSON](#async-http-get-then-parse-the-json-with-miro)
- [WebSocket with reconnect](#websocket-with-reconnect)
- [Download with progress and cancel](#download-with-progress-and-cancel)
- [Video playback](#video-playback-macos-and-windows)
- [WebView with a typed C++ API](#webview-with-a-typed-c-api-apple-and-windows)
- [ML](#ml-build-a-model-load-it-predict-macos)
- [Files and processes](#files-and-processes)

## Consuming it with CMake

Copy `CPM.cmake` from github.com/cpm-cmake/CPM.cmake into `cmake/`, then:

```cmake
cmake_minimum_required(VERSION 3.31)
project(MyApp VERSION 0.1.0 LANGUAGES C CXX)

if (APPLE)
    enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CPM.cmake)

CPMAddPackage(
        NAME eacp
        GITHUB_REPOSITORY eyalamirmusic/eacp
        GIT_TAG develop                       # pin a commit for real use
        OPTIONS "EACP_ENABLE_EXAMPLES OFF" "EACP_ENABLE_TESTS OFF")

add_executable(MyApp Main.cpp)
target_link_libraries(MyApp PRIVATE eacp-ui)   # or eacp-core, eacp-graphics ...
```

A Core-only consumer passes `"EACP_BUILD_GRAPHICS OFF"` too.
`-DCPM_eacp_SOURCE=$HOME/projects/eacp` builds against a local checkout.

On Linux (Debian/Ubuntu) the graphics stack needs:

```bash
sudo apt-get install build-essential ninja-build pkg-config libcurl4-openssl-dev \
    libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
    libxcb1-dev libxcb-xkb-dev libxkbcommon-x11-dev libxcb-randr0-dev \
    libxcb-xfixes0-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-xinput-dev \
    libfreetype-dev libharfbuzz-dev libfontconfig-dev fonts-dejavu-core \
    libvulkan1 mesa-vulkan-drivers
```

Tests and examples build only when eacp is the top-level project
(`EACP_ENABLE_TESTS`, `EACP_ENABLE_EXAMPLES`). Targets link their dependencies
publicly, so linking `eacp-ui` brings `eacp-graphics`, `eacp-gpu` and
`eacp-core`. `eacp-core` brings Miro (JSON and reflection).

Most modules have an umbrella header named after their directory under
`Lib/eacp/`: `<eacp/Core/Core.h>` (`eacp-core`), `<eacp/Network/Network.h>`
(`eacp-network`), `<eacp/Graphics/Graphics.h>` (`eacp-graphics`), and the same
for GPU, Text, UI, SVG, Sprites, GPUWidgets, Camera, CameraView, VideoView,
WebView and ML. Video has none: include its headers directly
(`<eacp/Video/Decode/Player.h>`). Some modules are several targets
(`eacp-network-json`, `eacp-gpu-codegen`, `eacp-ui-network`). Namespaces follow the
module (`eacp::Graphics`, `eacp::UI`, `eacp::GPU`); Core splits into
`eacp::Apps`, `Threads`, `Files`, `Processes`, `Time`, and Network into
`eacp::HTTP`, `WebSocket`. Examples write `using namespace eacp;`.

## Platform matrix

| Variable | Gates | Apple (macOS, iOS) | Windows | Linux | Android |
|---|---|---|---|---|---|
| (none) | Core, Network | yes | yes | yes | yes |
| `EACP_HAS_DRAW` | Graphics (windows, views, input) | yes | yes | yes | yes |
| `EACP_HAS_GPU` | GPU, GPUWidgets, Sprites | yes | yes | yes | yes |
| `EACP_HAS_TEXT` | Text, UI, SVG | yes | yes | yes | yes |
| `EACP_HAS_CONTEXT` | 2D `Graphics::Context`, `Font`, `View::paint` | yes | yes | no | no |
| `EACP_HAS_CAPTURE` | Camera; Video and VideoView on macOS and Windows only | yes (no Video on iOS) | yes | no | Camera only |
| `EACP_HAS_WEBVIEW` | WebView | yes | yes | no | no |
| `EACP_HAS_COREML` | ML (`eacp-ml`) | yes | no | no | no |

Code that must run everywhere draws with UI components (`UI::ComponentHost`)
or a `GPU::GPUView`, not `View::paint`.

## A window with a custom View (Apple and Windows)

```cpp
#include <eacp/Graphics/Graphics.h>

using namespace eacp;

struct ClickView : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        g.setColor({0.1f, 0.1f, 0.1f});
        g.fillRect(getLocalBounds());
        g.setColor({0.2f, 0.6f, 1.f});
        g.fillRect({last.x - 10.f, last.y - 10.f, 20.f, 20.f});
    }

    void mouseDown(const Graphics::MouseEvent& e) override
    {
        last = e.pos;
        repaint();
    }

    Graphics::Point last {50.f, 50.f};
};

int main()
{
    auto options = Graphics::WindowOptions();
    options.title = "Clicks";
    return Graphics::runWindowedApp<ClickView>(options);
}
```

## The same on every platform: UI components

A click counter: a `UI::Button` and a `UI::Label`, laid out in `resized()`.

```cpp
#include <eacp/UI/UI.h>

using namespace eacp;

struct Counter final : UI::Component
{
    Counter()
    {
        addChildren({button, label});
        button.onClick = [this]
        {
            ++clicks;
            label.setText("Clicks: " + std::to_string(clicks));
        };
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(20.f);
        button.setBounds(area.removeFromTop(40.f));
        area.removeFromTop(10.f);
        label.setBounds(area.removeFromTop(30.f));
    }

    UI::Button button {"Click me"};
    UI::Label label {"Clicks: 0"};
    int clicks = 0;
};

struct Host final : UI::ComponentHost
{
    Host() { setRootComponent(counter); }
    Counter counter;
};

struct MyApp
{
    Host host;
    Graphics::Window window {host};
};

int main() { return Apps::run<MyApp>(); }
```

## GPU drawing inside the tree: GPUComponent

An animated triangle, from `Apps/GPU/RotatingTriangle` moved into a component.
Pipelines are built in `targetChanged`, which runs again after a device
restore; `update` advances the angle; `render` draws one pass.

```cpp
#include <eacp/UI/UI.h>

using namespace eacp;
using namespace eacp::GPU;

struct Vertex
{
    Maths::Vec2 position;
    Maths::Vec3 color;
};

struct RotatingShader final : ShaderProgram
{
    RotatingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto color = varying(vertexInput(&Vertex::color));
        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        setPosition(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));
        setFragment(float4(color, 1.0f));
    }

    Uniform<Float> angle;
    EACP_SHADER(angle)
};

struct TriangleComponent final : UI::GPUComponent
{
    TriangleComponent()
    {
        shader.setVertices(vertices);
        setContinuous(true);
    }

    void targetChanged(Frame&) override
    {
        shader.prepare(sampleCount(),
                       hasDepth(),
                       PrimitiveTopology::Triangles,
                       BlendMode::None,
                       colorFormat());
    }

    void update(Threads::FrameTime time) override
    {
        angle += 1.2f * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        shader.angle = angle;
        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    static inline const Vertex vertices[] = {
        {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
        {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
        {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
    };

    RotatingShader shader;
    float angle = 0.0f;
};
```

Add it to a component tree like any other component.

## Text in a GPUComponent

Inside a component's `paint(UI::Graphics&)`, draw text with
`UI::Graphics::drawText`: it is laid out and cached for you. Use
`Text::TextRenderer` only when drawing into your own GPU pass (a
`UI::GPUComponent` or `GPU::GPUView`). Pattern from `Apps/UI/TouchDemo`:

```cpp
#include <eacp/Text/TextRenderer.h>
#include <eacp/UI/UI.h>

using namespace eacp;

struct Caption final : UI::GPUComponent
{
    Caption()
    {
        auto font = Text::Font();
        font.family = "Helvetica";
        font.pointSize = 18.f;
        text.setFont(font);
    }

    void targetChanged(GPU::Frame&) override { text.setSampleCount(sampleCount()); }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({});
        text.setViewport(frame.logicalSize(), frame.backingScale());
        text.begin();
        text.draw("hello", {10.f, 10.f + text.ascent()}, Graphics::Color {1.f, 1.f, 1.f});
        text.flush(pass);
    }

    Text::TextRenderer text;
};
```

The family is resolved by the platform (fontconfig on Linux); an unknown name
falls back to a default face.

## Timer every 100 ms, then stop it

A `Threads::Timer` ticks on the main thread until destroyed. Destroying it
inside its own callback is undefined, so defer that with `Threads::callAsync`.

```cpp
struct Ticker
{
    Ticker() { timer.emplace([this] { tick(); }, Time::MS {100}); }

    void tick()
    {
        if (++ticks == 10)
            Threads::callAsync([this] { timer.reset(); });
    }

    std::optional<Threads::Timer> timer;
    int ticks = 0;
};
```

Keep the `Ticker` alive (a member of the app struct) until the deferred reset
has run.

## Async HTTP GET, then parse the JSON with Miro

`HTTP::asyncRequest` runs on a worker thread and calls back on the main thread.

```cpp
#include <eacp/Network/Network.h>
#include <Miro/Reflect.h>

struct Slideshow
{
    std::string title;
    MIRO_REFLECT(title)
};

HTTP::asyncRequest(HTTP::Request("https://httpbin.org/json"),
                   [](const HTTP::Response& res)
                   {
                       if (!res.error.empty() || res.statusCode != 200)
                           return;

                       auto show =
                           Miro::createFromJSONString<Slideshow>(res.content);
                       LOG("title: ", show.title);
                   });
```

Miro's string functions (`<Miro/Reflect.h>`):

- `std::string Miro::toJSONString(const T& value, int indent = 0)` serialises
  a `MIRO_REFLECT` type.
- `T Miro::createFromJSONString<T>(std::string_view json)` and
  `void Miro::fromJSONString(T& value, std::string_view json)` never throw:
  malformed or empty input parses as null and loads no fields, so you get a
  default `T {}` (or `value` unchanged) and no error.
- `Miro::Json::parse(text)` (`<Miro/Json.h>`) throws on malformed JSON.
  When bad input must be reported, wrap it:
  `try { Miro::fromJSON(show, Miro::Json::parse(text)); } catch (const std::exception& e) { LOG(e.what()); }`.

Blocking request: `HTTP::Request(url).perform()`.

Round trip with a local type:

```cpp
struct Settings
{
    std::string name;
    int volume = 50;
    MIRO_REFLECT(name, volume)
};

auto text = Miro::toJSONString(Settings {"main", 80});   // {"name":"main","volume":80}
auto back = Miro::createFromJSONString<Settings>(text);  // back.volume == 80
```

## WebSocket with reconnect

A `WebSocket::Connection` never reconnects by itself. Rebuild it from
`onClose`, deferred with `Threads::callAfter`: replacing the Connection inside
its own callback would destroy it mid-delivery.

```cpp
struct Feed
{
    Feed() { connect(); }

    void connect()
    {
        auto callbacks = WebSocket::Callbacks();
        callbacks.onMessage = [](const WebSocket::Message& m) { LOG(m.data); };
        callbacks.onClose = [this](const WebSocket::CloseStatus&)
        { Threads::callAfter(Time::MS {2000}, [this] { connect(); }); };

        connection.emplace("wss://example.com/feed", callbacks);
    }

    std::optional<WebSocket::Connection> connection;
};
```

`Feed` must outlive the pending `callAfter`; hold it for the app's lifetime.

## Download with progress and cancel

`HTTP::asyncDownload` uses its own progress block, so the caller can't see
progress or cancel one download. For both, run `HTTP::downloadFile` on a
thread with `Request::progress` set; its atomics are safe to read and write
from any thread.

```cpp
struct Download
{
    Download()
        : worker([this]
                 {
                     auto req = HTTP::Request("https://example.com/big.zip");
                     req.progress = &progress;
                     auto res = HTTP::downloadFile(req, "/tmp/big.zip");
                     LOG("finished: ", res.statusCode, " ", res.error);
                 })
    {
    }

    ~Download() { progress.cancel = true; }   // the worker joins after this

    void logProgress()
    {
        LOG(progress.bytesReceived.load(), " of ", progress.totalBytes.load());
    }

    HTTP::DownloadProgress progress;
    Threads::Thread worker;
    Threads::Timer poll {[this] { logProgress(); }, Time::MS {250}};
};
```

Set `progress.cancel = true` from a button to stop it; `downloadFile` then
returns with `error` set.

## Video playback (macOS and Windows)

A `Video::FrameStream` decodes a file, a `Video::Player` runs its clock, and a
`Video::VideoView` draws it. The Player holds a reference to the stream and the
view holds neither, so declare stream, then player, then view: members are
destroyed in reverse, so the view goes first. Link `eacp-video-view`.

```cpp
#include <eacp/Graphics/Graphics.h>
#include <eacp/Video/Decode/Player.h>
#include <eacp/VideoView/VideoView.h>

using namespace eacp;

struct MyApp
{
    MyApp()
    {
        if (!stream.open(FilePath("/Users/me/clip.mp4")))
            return;

        view.attach(player);
        player.play();
    }

    Video::FrameStream stream;
    Video::Player player {stream};
    Video::VideoView view;
    Graphics::Window window {view};
};

int main() { return Apps::run<MyApp>(); }
```

## WebView with a typed C++ API (Apple and Windows)

Adapted from `Apps/WebView/WebViewSubApi` (which mounts this API nested; here it is the root). The API is a class with `MIRO_REFLECT_API`;
commands are its methods, events are `Miro::Event<T>` members.

```cpp
// Types.h
#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

struct GreetRequest { std::string name; MIRO_REFLECT(name) };
struct Greeting { std::string text; MIRO_REFLECT(text) };
struct Tick { int count = 0; MIRO_REFLECT(count) };

namespace Api
{
class GreeterApi
{
public:
    // Bodies inline: the codegen executable compiles this header alone.
    Greeting greet(const GreetRequest& req) { return {"hello " + req.name}; }

    void publishTick(int count) { ticks.publish(Tick {count}); }

    Miro::Event<Tick> ticks;

    MIRO_REFLECT_API(greet, ticks)
};
} // namespace Api

// App.h
#include "Types.h"
#include <eacp/WebView/WebView.h>

using namespace eacp;

struct MyApp
{
    Api::GreeterApi api;
    Graphics::WebView webView {Graphics::embeddedOptions("GreeterApp")};
    Graphics::WebViewBridge transport {webView, api};
    Graphics::Window window {webView};
};

// Main.cpp
#include "App.h"
int main(int argc, char* argv[]) { return eacp::Apps::run<MyApp>(argc, argv); }
```

Declaration order is the teardown order in reverse: the bridge goes before the
WebView, and the API last. The CMake function builds the Vite project in
`web/`, embeds it as resources under `CATEGORY` (the name passed to
`embeddedOptions`), and writes a typed TS client into `web/src/generated`:

```cmake
eacp_add_webview_app(Greeter
        APP_HEADER  App.h
        SOURCES     Main.cpp
        API         Api::GreeterApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    GreeterApp
        BUNDLE_ID   com.example.greeter
        BUNDLE_NAME "Greeter")       # add REACT for React hooks
```

The page side (`web/src/main.ts`, a plain Vite + TypeScript project):

```ts
import { backend } from './generated/backend';

backend.on?.('ticks', (tick) => console.log(tick.count));
const reply = await backend.greet({ name: 'world' });
document.body.textContent = reply.text;
```

`-DEACP_WEBVIEW_DEV=ON` serves the page from `npm run dev` instead of
embedding it.

## ML: build a model, load it, predict (macOS)

From `Apps/ML/Projection`. Building the package is portable; `ML::Model` is
Apple only (`eacp-ml`).

```cpp
#include <eacp/ML/ML.h>

using namespace eacp;

constexpr auto width = 4;

auto weights = Vector<float> {};
weights.resize(width * width, 0.5f);
auto zeros = Vector<float> {};
zeros.resize(width, 0.0f);

auto graph = ML::Graph {};
auto x = graph.input("x", {1, width}, ML::DType::float16);
auto weight = graph.halfConstant("weight", {width, width}, weights);
auto bias = graph.halfConstant("bias", {width}, zeros);
graph.output(graph.softmax(graph.linear(x, weight, bias), -1), "y");

if (!graph.isValid())
    LOG(graph.errors()[0]);

auto model = ML::Model {};
auto loaded = model.load(graph.build(), ML::Options {});
if (!loaded)
    LOG("load failed: ", loaded.error);

auto input = ML::MultiArray::create({1, width}, ML::DType::float16);
auto values = Vector<float> {};
values.resize(width, 1.0f);
input.fromFloats(values);
auto output = ML::MultiArray::create({1, width}, ML::DType::float16);

auto inputs = ML::Inputs {};
inputs["x"] = input;
auto outputs = ML::Outputs {};
outputs["y"] = output;

if (auto result = model.predict(inputs, outputs); result)
    for (auto value: output.toFloats())
        LOG(value);
```

## Files and processes

```cpp
auto options = Files::DirectoryOptions();
options.recursive = true;
options.pattern = "*.wav; *.flac";

for (auto& file: Files::listFiles(FilePath("/Users/me/Samples"), options))
    LOG(file.c_str());

auto result = Processes::run("git", {"status", "--short"});
if (result.launched && result.exitCode == 0)
    LOG(result.output);
```
