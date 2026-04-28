# eacp

A cross-platform C++20 framework that abstracts native OS primitives behind a
single, modern API. eacp lets you write desktop and mobile applications once
and have them target the platform's first-class primitives directly — no
heavyweight runtime, no third-party dependencies beyond the host OS frameworks.

> **⚠️ Alpha software.** eacp is in active early development. APIs are unstable
> and **will** change without notice between commits. Platform coverage is
> incomplete, edge cases are unhandled, and parts of the surface area are
> stubs. Do not use this in production. It is published in the open so that
> early adopters and contributors can experiment with it — expect breakage.

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
  native Windows graphics stack.
- **Widgets & menus** — native text inputs, menus, and embedded views.
- **WebView** — embed a system web view (WKWebView on Apple, WebView2 on
  Windows) with support for popups and new-window requests.
- **Networking** — an `HTTP::Request` / `HTTP::Response` API plus a small
  `HTTPServer`, backed by NSURLSession on Apple platforms.
- **SVG** — parsing and rendering of SVG documents into the graphics layer.
- **Interop helpers** — RAII wrappers (`Ptr<T>`, `CFRef<T>`,
  `AutoReleasePool`) for safe Objective-C / Core Foundation interop, plus
  generic utilities (`Pimpl`, `Singleton`, vector helpers).

## Supported platforms

| Platform | Status |
| --- | --- |
| macOS   | Primary target |
| Windows | Supported, less mature than macOS |
| iOS     | Partial — core, graphics, and views |

Linux/Android are not currently supported.

## A taste of the API

A minimal console app with a recurring timer:

```cpp
#include <eacp/Core/Core.h>

struct App
{
    void update()
    {
        eacp::LOG(std::to_string(numTimes));
        if (++numTimes == 4)
            eacp::Apps::quit();
    }

    int numTimes = 0;
    eacp::Threads::Timer timer {[&] { update(); }, 1};
};

int main()
{
    eacp::Apps::run<App>();
    return 0;
}
```

A GUI app embedding a web view:

```cpp
#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        webView.loadURL("https://example.com");
        window.setContentView(webView);
    }

    WebView webView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
```

An HTTP request:

```cpp
#include <eacp/Network/HTTP/Http.h>

auto req = eacp::HTTP::Request::post("https://api.example.com/posts", body);
req.headers["Content-Type"] = "application/json";
auto res = req.perform();
```

More examples live under [`Apps/`](Apps), including `SimpleView`, `HTTP`,
`HttpServer`, `WebViewEmbed`, `Todo`, and `SVG`.

## Building

eacp uses CMake (3.31+) and a C++20 toolchain. Dependencies are fetched via
[CPM](https://github.com/cpm-cmake/CPM.cmake) automatically at configure time.

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

- `EACP_UNITY_BUILD` (default `ON`) — compiles eacp libraries as unity builds
  for faster full-project compilation. Turn it `OFF` when working with
  language servers or tooling that consumes `compile_commands.json`:

  ```bash
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
  ```

## Repository layout

```
Lib/eacp/
  Core/       App lifecycle, threading, ObjC/CF interop, utils
  Graphics/   Windows, views, widgets, menus, drawing primitives
  Network/    HTTP client and minimal HTTP server
  WebView/    System web view embedding
  SVG/        SVG parsing and rendering
Apps/         Example applications
Tests/        Unit tests
CMake/        Build helpers (TargetSetup, CPM)
```

## Project status & contributing

eacp is alpha. Expect:

- Breaking API changes between commits.
- Missing features and platform gaps (notably on Windows and iOS).
- Rough edges in error handling, threading, and lifecycle management.

Issues, patches, and experiments are welcome, but please treat anything you
build on top of eacp today as throwaway code.

## License

MIT — see [LICENSE](LICENSE).
