# Building

eacp uses CMake (3.31+) and a C++20 toolchain. The top-level `README.md` has
the two commands a build needs; this page is everything behind them: the
dependencies, every option, the capability variables that decide which
modules a platform gets, the pieces that build everywhere, and how CI runs.

## Dependencies

Dependencies are fetched by [CPM](https://github.com/cpm-cmake/CPM.cmake) at
configure time — `ea_data_structures`, `Miro`, `ResEmbed`,
[`ESIMD`](https://github.com/eyalamirmusic/ESIMD) (the SIMD kernels behind
`Graphics`' image operations and the camera frame conversion, a library of its
own so other projects can take it without eacp; `CMake/FindESIMD.cmake`, the
`ESIMD` target, `-DCPM_ESIMD_SOURCE=$HOME/Code/ESIMD` for a local checkout) and,
behind `EACP_BUILD_SPIRV` and so on Linux only by default, `glslang`. A Linux build
adds `Vulkan-Headers`, `volk` and `VulkanMemoryAllocator`
(`CMake/FindVulkanBackend.cmake`, one `eacp-vulkan` target, fetched on no
other platform), libcurl for the HTTP client, and three pkg-config groups
found against the machine's own libraries: the Wayland client library,
`wayland-protocols` with `wayland-scanner`, xkbcommon and libdecor
(`CMake/FindWayland.cmake`); xcb with `xcb-xkb`, `xkbcommon-x11`,
`xcb-randr`, `xcb-xfixes`, `xcb-cursor`, `xcb-icccm` and `xcb-xinput` for the
X11 backend (`CMake/FindX11Backend.cmake`); and FreeType, HarfBuzz and
fontconfig for the glyph rasterizer (`CMake/FindLinuxText.cmake`). Nothing
links `libvulkan`: `volkInitialize()` opens it by name at runtime, so a
machine with no driver builds the same binary and reports `Device::isValid()`
false. The package names are in [Linux.md](Linux.md). An Android build
fetches nothing more and needs no libcurl: its HTTP and WebSocket clients are
Java's own, reached through JNI.

One dependency is carried in the tree instead: `ThirdParty/miniz`, the
amalgamated miniz 3.1.2 pair beside its MIT license, built as its own C
target so it never joins a unity build and its warnings are silenced. Only
`eacp-core` links it, PRIVATE, and only `Utils/Zip.cpp` includes its header,
so the whole of it is reached through `eacp::Zip`. To update it, replace the
files under `ThirdParty/miniz` and the version in its README.

## Options

- `EACP_UNITY_BUILD` (default `OFF`) — compiles eacp libraries as CMake unity
  builds, which is markedly faster for a cold full-project build. It is off by
  default because a unity build collapses per-file entries in
  `compile_commands.json`, which is what language servers read. Pass
  `-DEACP_UNITY_BUILD=OFF` explicitly when reconfiguring a build directory
  that may have cached `ON`.

- `EACP_BUILD_GRAPHICS` (default `ON`) — builds the whole drawing half. Turn it
  off to build only the portable modules, on any platform. It is the only
  switch that leaves the Linux backend out; there is no Linux-specific one.

- `EACP_BUILD_WEBVIEW` (default `ON`) — builds the native `WebView` module
  where the platform has one.

- `EACP_CI_BUILD` (default `OFF`) — the single switch that reproduces CI's
  exact configuration locally. It forces `EACP_UNITY_BUILD` and
  `MIRO_UNITY_BUILD` on and turns on `EACP_PCH`. Because it turns unity on, it
  is for reproducing CI, not for LSP-backed development.

  ```bash
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_CI_BUILD=ON
  ```

- `EACP_PCH` (default `OFF`, on under `EACP_CI_BUILD`) — shares one
  precompiled header, the STL, across every eacp target, which is worth
  roughly a third of the compile time of a typical translation unit. It holds
  no eacp header on purpose, but editing `CMake/Pch.h` still rebuilds the
  project, so it is off for normal work and on in CI, where every build is
  cold anyway. `<windows.h>` is deliberately not in it: CMake builds a PCH
  with `/FI`, so the payload is force-included into every translation unit,
  and windows.h brings two dozen macros with it — `near` and `far` among them,
  which collide with ordinary member names. Measured, it saves a portable TU
  nothing and costs the `*-Windows.cpp` TUs that do want it a flat ~68 ms
  each to parse it through `WinInclude.h` instead.

- `EACP_BUILD_SPIRV` (default `ON` on Linux, `OFF` elsewhere) — builds
  `eacp-spirv`, glslang wrapped as a GLSL-to-SPIR-V compiler (a shallow
  ~75 MB checkout, about 5 s of build on a laptop, a minute on a 4-core CI
  runner). It is on where the Vulkan backend ships it and off on macOS and
  Windows, whose backends compile their own dialects; passing `ON` there
  builds the compiler and turns the GLSL compile checks in `GPUCodegenTests`,
  `GPUTests` and `UITests` back on, which is how to check the emitter on a
  Mac. Consumers test `if (TARGET eacp-spirv)`. Passing `OFF` on Linux is
  only meaningful together with `-DEACP_BUILD_GRAPHICS=OFF`: the Vulkan
  backend has no shader compiler in the OS, so a Linux graphics build without
  it stops at configure time and says so.

- `EACP_WEBVIEW_DEV` (default `OFF`) — skips the Vite production build and
  resource embedding for webview apps. The UI is served from the Vite dev
  server instead (`npm run dev` in the app's `web/` dir); the runtime already
  prefers a reachable dev server (`Options::Embedded::preferDevServer`).
  Schema codegen still emits TS into `web/src/generated` on every app build.

- `EACP_VERBOSE_CONFIGURE` (default `OFF`) — prints the configure detail a
  clean configure leaves out: the FetchContent population of the
  `DOWNLOAD_ONLY` Vulkan sources, the pkg-config and `find_package` probes
  and the `check_*` results under them. CPM's own line per package prints
  either way. `CMake/ConfigureLog.cmake` is the whole of it. Warnings and
  errors sit above the log level, so nothing this hides is something that
  went wrong.

- `EACP_HIDDEN_VISIBILITY` (default `ON`) — compiles eacp with hidden symbol
  visibility.

- `EACP_ENABLE_TESTS` and `EACP_ENABLE_EXAMPLES` (default on when eacp is
  the top-level project) — build `Tests/` and `Apps/`. A project consuming
  eacp through CPM gets neither unless it asks.

## Android

`-DCMAKE_SYSTEM_NAME=Android` configures with the NDK
`EACP_ANDROID_NDK_VERSION` locks to, else the one `$ANDROID_NDK_HOME` (or
`$ANDROID_NDK_ROOT`, `$ANDROID_NDK`) names, else the newest NDK in Android
Studio's SDK or `$ANDROID_HOME` (`CMake/AndroidToolchain.cmake`), for
`arm64-v8a` at `EACP_ANDROID_MIN_SDK` unless `-DANDROID_ABI` or
`-DANDROID_PLATFORM` say otherwise. An explicit `-DCMAKE_TOOLCHAIN_FILE` is used as given. The NDK is installed with Studio's
SDK Manager; eacp installs nothing.

The configure writes an Android Studio project, which is how an app is built,
installed, run, debugged, signed and bundled for Google Play. The project is a
Gradle one with one module per `eacp_add_app`, whose `externalNativeBuild`
runs this `CMakeLists.txt` for that one target per ABI, given every
non-internal `EACP_*` and `CPM_*` cache variable of the generating configure
plus `CPM_SOURCE_CACHE`, so a module builds the same sources with the same
options. The SDK is found once — `$ANDROID_HOME`, `$ANDROID_SDK_ROOT`, else
Studio's default location — and the toolchain and the project use the same
one. `CMake/Android.cmake` writes it from the
templates and the Gradle wrapper in `CMake/Android/`.

- `EACP_ANDROID_STUDIO` (default `ON`) — write the project.
- `EACP_ANDROID_STUDIO_DIR` (default `<build>/AndroidStudio`) — where.
- `EACP_ANDROID_ABIS` (default `arm64-v8a;x86_64`) — the ABIs its modules
  build; the second is the emulator on an Intel host.
- `EACP_ANDROID_CLEARTEXT_TRAFFIC` (default `OFF`) — the manifest's
  `usesCleartextTraffic`, which decides whether `http://` and `ws://` reach
  any host, loopback included. Every manifest asks for `INTERNET` either way.
- `EACP_ANDROID_SDK` (default: the SDK found above) — Gradle's `sdk.dir`.
- `EACP_ANDROID_MIN_SDK` (default `33`) — `minSdk` and the level a configure
  compiles for: eacp's floor, an app's decision. The NDK carries every level's
  sysroot, so nothing installed decides it.
- `EACP_ANDROID_TARGET_SDK` (default `35`) — `targetSdk`.
- `EACP_ANDROID_COMPILE_SDK` (default empty) — `compileSdk`. Empty is the
  newest platform under `<sdk>/platforms`, previews skipped; with none
  installed it is the target level, with a warning, and Gradle downloads it on
  sync once the SDK licenses are accepted. Below the target level fails the
  configure.
- `EACP_ANDROID_GRADLE_PLUGIN` (default `9.4.1`) and `EACP_ANDROID_GRADLE`
  (default `9.8.0`) — the Android Gradle Plugin and Gradle, a tested pair an
  app may raise. Both are downloaded, not installed, so nothing on the machine
  picks them.

The module's `ndkVersion` is the NDK the configure compiled with, so a
project's own toolchain file carries through to Studio.

Gradle runs the CMake that ran the configure and looks for Ninja beside it,
in the SDK's own CMake package, or on its PATH. `ResEmbed`'s generator is
built for the host inside each module's configure, as on every cross build,
so the host needs a C++ compiler; the Ninja Gradle found is handed to that
configure too, so Studio started from the Dock, with no shell `PATH`, builds.
[`Apps/Android/README.md`](../Apps/Android/README.md) is the walkthrough.

`Core` reaches the framework through JNI where Android has no C API
(`Core/Android/Jni.h`), with the application `Context` from
`Jni::setContext`, else `ActivityThread.currentApplication()`. `Clipboard` is
`ClipboardManager` (`App/Clipboard-Android.cpp`): text only, `copyFiles`
returns false, and a read is empty while the app lacks focus.
`Apps::openExternalURL` is an `ACTION_VIEW` intent; the file pickers return
`nullopt` at once, a Storage Access Framework picker needing activity-result
plumbing NativeActivity does not have. `FilePath::appDataDirectory()` is
`getFilesDir()` and `cacheDirectory()` is `getCacheDir()`, so
`appSupportDirectory()` and `appCacheDirectory()` land under each.
`Files::executablePath()` is the app's own `lib<Target>.so` (the process is the
zygote's `app_process64`), so the fallback app name is the target's;
`resourcesDirectory()` and `getBundleResourcePath()` are empty in an app,
whose resources are APK assets with no path, readable only through
`AAssetManager`, which eacp does not wrap yet. A binary run from `adb shell` is its own executable, with its
resources beside it, as on desktop Linux.

## A local Miro source

Miro is fetched via CPM from `eyalamirmusic/Miro` by default. To work against
a local checkout while co-developing both repos, pass
`-DCPM_Miro_SOURCE=$HOME/Code/Miro` at configure time; CPM honours
`CPM_<Name>_SOURCE` and uses the local path instead of the fetch.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DCPM_Miro_SOURCE=$HOME/Code/Miro
```

Use `$HOME`, not `~`. CMake does not expand `~`, and shell tilde expansion is
suppressed inside quotes, so `-DCPM_Miro_SOURCE="~/Code/Miro"` silently
configures against a non-existent path and fails later with errors like
`Unknown CMake command "miro_add_type_export"`.

## App targets

An app bundle is set up with the functions in `CMake/TargetSetup.cmake`, which
a project that fetches eacp has as well:

- `set_default_target_setting(target)` — the warning level, LTO in Release,
  and on Apple the bundle's `Info.plist` from eacp's template
  (`CMake/macOSBundleInfo.plist.in`, or the iOS one), unless the target
  already has one.
- `eacp_set_gui_subsystem(target)` — a windowed app on Windows, and the app's
  name and version stamped into the binary.
- `eacp_set_app_icon(target IMAGE ...)` — the at-rest icon.
- `eacp_add_plist_entries(target key value ...)` — keys added to the
  template's plist, for the usage description macOS wants before it grants
  the camera or the microphone, or `LSUIElement` for a menu-bar app. `TRUE`
  and `FALSE` become booleans, anything else a string. Calls accumulate, and
  may come before or after `set_default_target_setting`.

```cmake
eacp_add_plist_entries(MyApp
        NSMicrophoneUsageDescription "MyApp listens to transcribe what it hears."
        LSUIElement TRUE)
```

The generated template lands in the target's binary directory as
`<target>-Info.plist.in`. An app that needs more than key-value entries sets
`MACOSX_BUNDLE_INFO_PLIST` to a template of its own, which
`set_default_target_setting` leaves alone. The template paths are
`EACP_MACOS_PLIST` and `EACP_IOS_PLIST`, published when `TargetSetup` is
included, so a project that fetches eacp reads them without running
`eacp_default_setup()`.

## Capability variables

The top-level `CMakeLists.txt` decides this once, in seven capability variables
that `Lib`, `Apps` and `Tests` all read rather than restating the platform test.
The three drawing ones are on together on every platform that draws — they
stay three nested variables because each gates a different set of modules, and
a new port reaches them one at a time; the next three hang off
`EACP_HAS_DRAW` and are Apple/Windows-only, and `EACP_HAS_COREML` hangs off
`EACP_HAS_GPU` and is Apple-only. `Network` needs none of them: it is
unconditional, over NSURLSession and Network.framework on Apple, WinHTTP on
Windows, libcurl on Linux and Java's `HttpURLConnection` and sockets through
JNI on Android, and the WebView page bridge over its RPC, `eacp-ui-network`
and their tests build with it:

| Variable | On when | Gates |
| --- | --- | --- |
| `EACP_HAS_DRAW` | `EACP_BUILD_GRAPHICS`, and Apple, Windows, Linux or Android | `Graphics` — `EmbeddedView` with it, embedding being a windowing feature rather than a drawing one — and `Tests/Graphics` |
| `EACP_HAS_GPU` | `EACP_HAS_DRAW`, and Apple, Windows, Linux or Android | `GPU`, `GPUWidgets`, `Sprites`, their tests, `Apps/GPU` and `Apps/Plugins` |
| `EACP_HAS_TEXT` | `EACP_HAS_GPU`, and Apple, Windows, Linux or Android | `Text`, `UI`, `SVG`, their tests, `Apps/UI` and the GPU examples that draw glyphs |
| `EACP_HAS_CONTEXT` | `EACP_HAS_DRAW`, and Apple or Windows | the platform's own 2D tier: `Graphics::Context`, `Font`, `TextMetrics`, `TextInput`, the retained layers and layer views, the image codecs — and so `SVGBuilder`, `Apps/Graphics`, `Apps/SVG` and the examples that paint a 2D overlay |
| `EACP_HAS_CAPTURE` | `EACP_HAS_DRAW`, and Apple or Windows | `Camera`, `CameraView`, `Video`, `VideoView` |
| `EACP_HAS_WEBVIEW` | `EACP_HAS_DRAW` and `EACP_BUILD_WEBVIEW`, and Apple or Windows | the native `WebView` (WKWebView / WebView2) |
| `EACP_HAS_COREML` | `EACP_HAS_GPU`, and Apple | `eacp-ml`, the Core ML runner, `MLTests` and `Apps/ML` |
`EACP_HAS_CONTEXT` is also a compile definition on `eacp-graphics`, so the
`Graphics.h` umbrella leaves the 2D-tier headers out where it is off and a
caller reaching one fails to compile rather than to link. `EACP_HAS_COREML` is
one on `eacp-ml` in the same way.

## The pieces that build everywhere

Four pieces of the gated modules are portable and so sit outside all seven:
they are built and tested on every platform, Linux included, because none
touches a device. `eacp-gpu-codegen` is the shader EDSL and the MSL, HLSL and GLSL
emitters — string generation with no GPU under it, checked by
`GPUCodegenTests`. `eacp-cpu-compute` runs the kernels that EDSL records on the
CPU, checked by `CpuComputeTests` and timed by `CpuComputeBench`; it is also
what gives `GPUCodegenTests` and `GPUTests` a numeric half that runs with no
device, so the Linux lanes without a driver check what a kernel computes, not
only that its GLSL compiles. `eacp-webview-bridge` is the page bridge over a
`ScriptHost` rather than over a web view, checked by `ScriptHostTests`. And
`eacp-ml-graph` is the graph builder and the MIL, protobuf and blob writers —
bytes in and bytes out, with no Core ML under it — checked by `MLGraphTests`.

A fifth, `eacp-spirv`, wraps glslang as a GLSL-to-SPIR-V compiler
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

## Continuous integration

`.github/workflows/build.yml` builds and runs the test suite on macOS
(universal), Windows x64 and ARM64 (MSVC and clang-cl) and Linux (GCC, Clang,
and a Clang lane that runs the graphics backend on lavapipe under a headless
Weston and then under an Xvfb — all three build it, one has a device, a
compositor and an X server to run it on), and builds iOS for the simulator
and Android: an Ubuntu lane installs a pinned NDK with `sdkmanager`, configures
against it with `-DCMAKE_SYSTEM_NAME=Android` and builds `HelloGPU` and
`HelloNetwork` for `arm64-v8a`, which is every module an Android app links,
the network backend included, then configures once
more with no NDK named, which exercises the newest-installed path. Every lane configures with `EACP_CI_BUILD=ON`.

The Linux lanes install the packages listed in [Linux.md](Linux.md), and the
`Dockerfile` reproduces their three steps — a headless build, the suite under
a headless Weston, and the window and present suites again under an Xvfb:

```bash
docker run --rm -e EACP_HEADLESS=1 -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 \
    -v "$PWD":/workspace eacp-ci-linux \
    ci-build -DEACP_UNITY_BUILD=OFF

docker run --rm -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_DISPLAY=1 \
    -e EACP_REQUIRE_FONTS=1 -v "$PWD":/workspace eacp-ci-linux \
    with-weston ctest --test-dir build-ci-linux --output-on-failure \
    -E '^(X11|EmbeddedView)/'

docker run --rm -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 -e EACP_REQUIRE_DISPLAY=1 \
    -v "$PWD":/workspace eacp-ci-linux \
    with-xvfb ctest --test-dir build-ci-linux --output-on-failure \
    -R '^(X11|EmbeddedView|Present)/'
```

The environment variables those commands set — `EACP_VK_SOFTWARE`,
`EACP_REQUIRE_GPU`, `EACP_REQUIRE_DISPLAY`, `EACP_REQUIRE_FONTS`,
`EACP_HEADLESS` — are described in [Linux.md](Linux.md) and in the "Running
it" section of `Lib/eacp/GPU/README.md`.
