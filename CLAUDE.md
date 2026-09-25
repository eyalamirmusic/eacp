# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Rules

Claude must never commit or push without explicit permission from the user in
the current conversation.

## Project Overview

eacp is a cross-platform GUI/graphics framework written in modern C++20 with Objective-C++ interop. It provides abstractions for application lifecycle, graphics rendering, threading, GPU, and networking.

Platform coverage splits on whether a module draws, decided once in the
top-level `CMakeLists.txt` by seven capability variables that `Lib`, `Apps` and
`Tests` read instead of restating the platform test: `EACP_HAS_DRAW`
(`Graphics` — `EmbeddedView` with it, since embedding is a windowing feature
rather than a drawing one — and `Tests/Graphics`), `EACP_HAS_GPU` (`GPU`,
`GPUWidgets`, `Sprites`, `Apps/GPU`, `Apps/Plugins`), `EACP_HAS_TEXT` (`Text`, `UI`, `SVG`,
`Apps/UI` and the GPU examples that draw glyphs), `EACP_HAS_CONTEXT` (the
platform's own 2D tier — `Graphics::Context`, `Font`, `TextMetrics`,
`TextInput`, the retained `ShapeLayer`/`TextLayer` and their views, the image codecs — and so
`SVGBuilder`, `Apps/Graphics`, `Apps/SVG`, `Apps/UI/SVGDocument` and the GPU
examples that paint a 2D overlay), `EACP_HAS_CAPTURE` (`Camera`,
`CameraView`, `Video`, `VideoView`, the last two additionally off on iOS),
`EACP_HAS_WEBVIEW` (the native `WebView`) and `EACP_HAS_COREML` (`eacp-ml`,
the Core ML runner, `MLTests` and `Apps/ML`). The first three are
`APPLE OR WIN32 OR LINUX` — everywhere graphics builds at all — and stay
nested (`TEXT` implies `GPU` implies `DRAW`) because each gates a different
set of modules and a new port reaches them one at a time. The next three hang off
`EACP_HAS_DRAW` and are Apple/Windows-only, so on those two platforms the first six
are simply what `EACP_HAS_DRAW` alone used to decide; `EACP_HAS_COREML` hangs
off `EACP_HAS_GPU` and is Apple-only, and is a PUBLIC define on `eacp-ml`.
`Core`, `Network` and `SIMD` build everywhere, Linux included, and so do four
device-free pieces of the gated modules: `eacp-gpu-codegen`, the shader EDSL
and the MSL/HLSL/GLSL emitters (`GPUCodegenTests`); `eacp-cpu-compute`, an
interpreter that runs the same compute kernels on the CPU with no device
(`CpuComputeTests`, `CpuComputeBench`), which `GPUCodegenTests` and `GPUTests`
also link so their compute cases carry a CPU half that never self-skips —
every lane, the driverless Linux ones included, checks a kernel's numbers, and
one with a device cross-checks the emitted kernel against the interpreter;
`eacp-webview-bridge`, the page bridge over a `ScriptHost` (`ScriptHostTests`);
and `eacp-ml-graph`, the graph builder and the MIL/protobuf/blob writers
(`MLGraphTests`). `eacp-spirv` (`GPU/Spirv/`)
wraps glslang as a GLSL-to-SPIR-V compiler (`SpirvTests`); it is built on
Linux only by default (`EACP_BUILD_SPIRV`), because only the Vulkan backend
ships it, and macOS and Windows can opt in. Where it is built, every GLSL
source the codegen tests emit — and every hand-written GLSL twin in `GPUTests` —
is compiled by glslang inside the suite, so an emitter regression fails on every
Linux CI lane, the two with no Vulkan device included, rather than only as a
wrong pixel on the lane that has a device. See the table in `README.md`. CI
builds and tests macOS, Windows (x64 and ARM64, MSVC and clang-cl) and Linux
(GCC, Clang, and a Clang lane that runs the Vulkan backend on Mesa's lavapipe,
with the tests inside a headless Weston session and then the window and present
ones again inside an Xvfb, so windows and swapchains are real on both window
systems; all three Linux lanes build the whole graphics stack and install the
stock font packages so the text suites resolve rather than skip, and only the
third has a driver, a compositor and an X server to run the GPU and window
tests on), and builds iOS for the simulator.

Dependencies are fetched by CPM at configure time — `ea_data_structures`, `Miro`,
`ResEmbed` and, behind `EACP_BUILD_SPIRV` and so on Linux only by default,
`glslang`; a Linux build adds
`Vulkan-Headers`, `volk` and `VulkanMemoryAllocator` (`CMake/FindVulkanBackend.cmake`,
one `eacp-vulkan` target, fetched on no other platform). Plus libcurl on Linux,
which backs the HTTP client there, and — on Linux, where none of them are
optional — three pkg-config groups: the Wayland client library,
`wayland-protocols` with `wayland-scanner`, xkbcommon and libdecor (`CMake/FindWayland.cmake`, one
`eacp-wayland` target holding the generated protocol code), xcb with
`xcb-xkb`, `xkbcommon-x11`, `xcb-randr`, `xcb-xfixes`, `xcb-cursor`,
`xcb-icccm` and `xcb-xinput` for the X11 backend
(`CMake/FindX11Backend.cmake`, one `eacp-x11`
target; no Xlib symbol anywhere), and FreeType, HarfBuzz and fontconfig for the
glyph rasterizer (`CMake/FindLinuxText.cmake`, one `eacp-linux-text` target). Nothing links `libvulkan`: `volkInitialize()`
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

Linux draws through Wayland or X11, Vulkan and FreeType, and it is on wherever
the graphics modules are — `EACP_HAS_DRAW`, `EACP_HAS_GPU` and `EACP_HAS_TEXT`
are true there exactly as they are on Apple and Windows. `EACP_HAS_CONTEXT` is
not: there is no 2D backend.

`eacp-graphics` is split along a window-system seam, and both backends are
compiled into every copy: which one a window gets is a runtime decision, not a
build one. The neutral half: `Window/LinuxWindowSystem-Linux.{h,cpp}` decides
the preferred backend once per copy (`EACP_WINDOW_SYSTEM=wayland|x11`
overrides; else a plugin copy — `Platform::isDLL()` — takes X11, a standalone
app takes Wayland when a compositor answers and X11 otherwise;
`EACP_HEADLESS=1` gives none) and answers the process-wide questions with no
window to hang off — the primary output's frame, scale and refresh for
`Display-Linux.cpp` and `DisplayLink-Linux.cpp`, the seat behind `linuxSeat()`,
and clipboard installation for the preferred backend alone. `Window-Linux.cpp`
is option handling plus a `makeWindowNative()` switch on that preference between
`WaylandWindowNative`, `X11WindowNative` and an explicit headless native
(`Window/HeadlessWindow-Linux.cpp`, which is also what a preferred backend that
cannot connect falls back to); each implements `LinuxWindowNative` and holds a
`LinuxWindowState` beside it (`Window/LinuxWindowNative-Linux.{h,cpp}`: the
option-derived state and the behaviour no window system decides — constraints,
`resizeTo`, activation, `closeRequested`). A native is that interface *and* its
window system's `LinuxWindowSurface` (`Window/LinuxWindowSurface-Linux.h`:
content view, size, scale, mapped, focus and connection-lost callbacks, a
`NativeSurfaceHandle`, and the window's `ViewSurfaceBackend`), which is why the
interface does not derive from the surface. `View-Linux.cpp` keeps the per-view
records, the sync on bounds/visibility/add/remove, deferred repaint and
origin-in-window, and asks the window's `ViewSurfaceBackend`
(`View/ViewSurfaceBackend-Linux.h`) for a `ViewSurfaceNative` child that
knows only how to apply geometry and request a frame; `ViewSurface` in
`View-Linux.h` carries a tagged `NativeSurfaceHandle` (`Kind::None|Wayland|X11`,
connection, surface, window id) instead of Wayland pointers.
`Window/LinuxInput-Linux.{h,cpp}` holds the input state machines that are not
protocol — xkbcommon keymap/state and the UTF-8 for a key, key repeat, click
counting and double-click slop, held button and drag origin, wheel
accumulation, cursor shape — `Window/LinuxSeat-Linux.h` the seat interface both
`WaylandInput` and `X11Input` implement (polled key state, modifiers, keyboard
focus, the pointer's window and position, cursor refresh), so
`Graphics/Keyboard-Linux.cpp` and the pointer questions a view asks go through
`linuxSeat()` and include no Wayland header, and `Graphics/Keyboard-Linux.h` the
evdev-to-`KeyCode` table (`linuxKeyCodeFromEvdev`; X11 keycodes are evdev + 8,
so one table serves both backends).

The Wayland half is one process-wide connection
(`Window/WaylandDisplay-Linux.cpp`: registry, outputs, libdecor context, the
surface-to-window map, and the loop source that pumps it through
`Threads::addLoopSource` with a pre-poll flush), a native that is a
`wl_surface` under a libdecor frame with a viewport-stretched shm buffer behind
the content (`Window/WaylandWindow-Linux.cpp`), a `wl_subsurface` per presenting
view (`View/WaylandViewSurface-Linux.cpp`, the Wayland `ViewSurfaceBackend`),
seat protocol glue feeding the shared state machines with pointer-constraints
for mouse lock (`Window/WaylandInput-Linux.cpp`), the clipboard as a
`wl_data_device` on the seat (`Window/WaylandClipboard-Linux.cpp`, installed
into `Core`'s `Clipboard` through the backend hook in
`Core/App/Clipboard-Linux.h` so `eacp-core` links no Wayland; a copy needs
keyboard focus on one of our windows), a compositor disconnect that fires
`onLost` on every view surface and leaves the process headless, and stubs for
image codecs, menus, tray and system appearance.

The X11 half is its twin, on xcb with no Xlib symbol anywhere (`plan.md` D2).
`Window/X11Connection-Linux.{h,cpp}` is one connection per copy, opened lazily
and deliberately *not* gated on the preference — stage 3's `EmbeddedView` is
X11 whichever backend a toplevel prefers: the atoms interned in one batch, XKB
through `xkbcommon-x11`, RandR for the primary output (the root's live geometry
and no refresh where there is no mode, as on Xvfb) with
`XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE|CRTC_CHANGE|OUTPUT_CHANGE` selected on the
root so a screen resize or a mode change drops that cached output, reads it
again and re-rates the frame pacer, XFixes, an `xcb-cursor` context,
`PROPERTY_CHANGE` on the root for the `RESOURCE_MANAGER` that carries the
display's scale and cursor theme,
the id-to-target map that routes an event to its toplevel and, where the id is
a view's child window, to the view beside it, and the loop source whose
`prepare` drains `xcb_poll_for_queued_event` — the events Mesa's WSI pulled off
the socket it shares with us — before flushing. `xcb_connection_has_error`
after either step is connection loss, and fires `onConnectionLost` on every
window once. `Window/X11Window-Linux.cpp` is the toplevel:
`WM_PROTOCOLS`/`WM_DELETE_WINDOW`, `_NET_WM_NAME` with `WM_NAME` under it, a
`WM_CLASS` of `eacp`, `_NET_WM_PID`, `WM_NORMAL_HINTS` (minimum size, a fixed
size when not resizable, aspect), `_MOTIF_WM_HINTS` for borderless, a
`_NET_WM_STATE` toggle for maximise and `WM_CHANGE_STATE` for iconify, `mapped`
taken from the `MapNotify` rather than from the request, `ConfigureNotify` into
a resize and a real `onMoved` through `xcb_translate_coordinates` — positions
are real here, unlike Wayland — and `FocusIn`/`FocusOut` into activation. The
scale of a toplevel is `Xft.dpi` over 96, read from the root's
`RESOURCE_MANAGER` and kept as a fraction rather than rounded to a whole factor
(Qt's rule, not GTK's), so 144 is 1.5; the server measures a toplevel in pixels
and everything above the native in points, and `X11Window-Linux.cpp` converts
and rounds once at that crossing — the window's size and position on the way
out, the `ConfigureNotify` and the translated position on the way back,
`WM_NORMAL_HINTS` in pixels, and `Display` halved into points with the scale
reported as its `backingScale`. A `PropertyNotify` for `RESOURCE_MANAGER` re-reads
it and tells every toplevel, which keeps the point size it was laid out at,
asks the server for the pixels that size now needs and fires
`backingScaleChanged` down its content tree — the Wayland scale change exactly.
An `EmbeddedView`'s scale is still only what `setPixelsPerPoint` said.
`View/X11ViewSurface-Linux.cpp` makes a presenting view an `xcb_create_window`
child of the toplevel selecting `EXPOSURE` only, so pointer and key events
propagate up to the toplevel already in its coordinates, and paces frames from
a process-wide `X11FramePacer` — a `Threads::Timer` at the RandR mode's rate,
60 Hz where there is none, rebuilt at the new rate when a RandR change moves it
(a `Timer`'s interval is fixed at construction), dropped as soon as nothing is
armed, so no pacing thread outlives the last presenting view —
firing `onFrameDone` where Wayland has `wl_surface.frame`: a pacer, not a
compositor signal (`plan.md` D7). `Window/X11Input-Linux.cpp` feeds the
pointer and key events into the shared state machines, with the keymap taken
from the server (`xkb_x11_keymap_new_from_device`) and kept current through the
XKB state, map and new-keyboard events, so layouts and dead keys behave as they
do on Wayland. Repeat is the server's own — detectable auto-repeat is asked for
and a press of an already-pressed key is marked `isRepeat`, so `KeyRepeat` is
not used here — the cursor is `xcb_cursor_load_cursor` over the shared
`linuxCursorNames` applied to the toplevel, and a `ButtonPress` in an unfocused
mapped window takes focus with `xcb_set_input_focus`. The pointer half is
XInput 2 where the server has 2.1 or better: `X11Connection` negotiates it once
(`EACP_X11_NO_XI2=1` refuses it), a window of ours selects
`XI_ButtonPress/Release/Motion/Enter/Leave` for `XIAllMasterDevices` and leaves
the core pointer bits out of its own mask — the union the two would otherwise
form is a press delivered twice on some servers — and every
`XCB_GE_GENERIC` with the extension's opcode goes straight to the seat, which
is the only thing that knows what a valuator means. Scrolling is then a
valuator rather than buttons 4–7: `xcb_input_xi_query_device` is read for each
device's `ScrollClass` and read again on `XI_DeviceChanged`/`XI_Hierarchy`, a
per-axis last value makes each report a difference (the first after a change is
a baseline and scrolls nothing), the difference over the class's increment is
clicks, and the buttons 4–7 the server emulates beside it carry
`XIPointerEmulated` and are dropped. X11 measures scroll in clicks and never in
pixels, so a frame is always lines — fractional ones from a trackpad — and
`preciseScrolling` stays false; a device with no scroll class at all, which is
what a virtual pointer under Xvfb or in a VM is, still arrives as buttons 4–7,
now as XI2 button events with no emulated flag. Mouse lock is
`xcb_grab_pointer` plus `xcb_xfixes_hide_cursor` plus a warp to the centre,
held only while the window has keyboard focus, and what it reports is
`XI_RawMotion` selected on the root for the length of the lock: the device's
own report, delivered whoever holds the pointer grabbed, and made by no warp —
so the recentring warp, which a position could never be told apart from the
hand, contributes nothing and the motion path stops counting deltas while raw
events are live. Both figures a raw event carries are used, exactly as
Wayland's relative pointer gives both: `axisvalues` (accelerated) is
`MouseEvent::delta` and `axisvalues_raw` (the driver's own, before any pointer
curve) is `rawDelta`, which is what aiming a camera wants. The grab's
`owner_events` is off for that reason and no other: with it on the server hands
the same raw event to the root's selection twice — once on the normal delivery
up from the window under the pointer and once in the delivery to every root a
raw event always gets — and every delta would be doubled. Without XI2 all of
this falls back to the core pointer path unchanged, wheel buttons and
warp-measured deltas included, and that path is a test of its own. The cursor
theme and its size are
xcb-cursor's to read out of the same `RESOURCE_MANAGER` (`Xcursor.theme`,
`Xcursor.size`, and a size derived from `Xft.dpi` when neither names one,
plus `XCURSOR_PATH` and `XCURSOR_SIZE` from the environment — but not
`XCURSOR_THEME`, which xcb-util-cursor does not look at), and it reads them
only at `xcb_cursor_context_new`, so a `RESOURCE_MANAGER` change frees the
context and the cursors cached from it and builds both again. The clipboard is
the `CLIPBOARD` selection (`Window/X11Clipboard-Linux.{h,cpp}`, owned by the
connection and installed into `Core`'s `Clipboard` through the same backend hook in
`Core/App/Clipboard-Linux.h` the Wayland one uses), and where Wayland needs the
serial of an input event on a focused surface of ours, here the owner is a 1x1
`InputOutput` window made on first use and never mapped — so a plugin copy
with no toplevel and no keyboard focus can still copy. A copy takes the
selection with `xcb_set_selection_owner` and checks with
`xcb_get_selection_owner` that it took; a `SelectionRequest` is answered with
`TARGETS`, `UTF8_STRING`, `text/plain;charset=utf-8`, `text/plain`, `STRING`
and `TEXT` for text and
`text/uri-list` for files (the list built by `linuxUriList`, which both
backends now share), honouring a requestor that names no property and refusing
`MULTIPLE` and everything else with a `SelectionNotify` naming none; a
`SelectionClear` drops the store. A read is `xcb_convert_selection` into one
property on that window followed by `X11Connection::dispatchUntil`, the loop
source's own drain-poll-dispatch run by hand for a bounded 2s, so a window whose
`ConfigureNotify` lands beside the answer still gets it; our own selection is
answered from the store with no round trip, since the conversion would be served
by the thread waiting for it. `TARGETS` is asked first and decides both
`hasText` and which target to convert, an owner that will not answer it holds
nothing, and an `INCR` reply is read chunk by chunk off the `PropertyNotify`s.
Sending `INCR` is not implemented: a selection larger than one request to the
server is refused rather than truncated.

Embedding is the other half of the X11 backend and the reason for it.
`Window/EmbeddedView-Linux.cpp` is an `EmbeddedView::Native` that is an
`X11WindowSurface` and nothing more — no `LinuxWindowNative`, no options, no
`WindowEvents`, because there is no `Window` above it and the host decides what
a toplevel decides for itself. The child is an `xcb_create_window` InputOutput
window of the host's id, `XCB_COPY_FROM_PARENT` in depth and visual, with no
background pixmap so the host's own paint stands until ours does, and the same
`x11WindowEventMask` a toplevel selects (now in `X11Connection-Linux.h`, so both
take it from one place); being an `X11WindowSurface` is the whole of the
integration, so a `GPUView` inside presents through the same child-window path
and the seat reaches the content through the same `X11Input`. The host's window
is watched, never owned: `StructureNotify` is selected on it — unless it is a
window of this copy's, whose own mask that would replace — and the id is kept in
a second list on the connection (`watchForeignWindow`, dispatched after the
window's own target) rather than in the id map, which would take that window's
routing away from it. The parent's `ConfigureNotify` sizes the surface until
`setBounds` is first called and never after, a `ReparentNotify` moves the watch,
and only the child's own `DestroyNotify` is acted on, since the server hands a
departed client's ids straight back out; a notice the server sent rather than
one we asked for sets `X11WindowSurface::inferiorsGone`, so the view children
the server reaped with it are unregistered without a `DestroyWindow` each. With
no window at all — no connection, `EACP_HEADLESS=1`, a host id of 0 — the
surface is surfaceless and its content is still laid out at its point size,
like a headless toplevel. `setPixelsPerPoint` is the whole of the
scale (0 means 1 — X11 has none to read) and everything follows from it: the
pixels the surface covers, rounded outwards so no seam of the host's window
shows, the points its content is laid out in, and the event positions
`X11Input` now divides by the window's scale. Keyboard focus is the toplevel's
rule unchanged — a click inside takes it with `xcb_set_input_focus` and nothing
gives it back, which is what every Linux plugin does.
`Tests/Graphics/EmbeddedViewTests-Linux.cpp` is the suite: 17 cases in a binary
with a plain `main` that plays host itself, its own xcb connection owning the
parent window and eacp driven only through `getEventLoopFd()`/`pumpEventLoop()`,
2 of them seat-driven and so Xvfb-only;
`Present/anEmbeddedViewPresentsIntoItsHost` in `Tests/GPU` is the swapchain
half. `Apps/Plugins/X11Host` and `Apps/Plugins/X11Plugin` are the in-tree DAW
stand-in: a host app whose window id crosses a four-function C ABI
(`Apps/Plugins/X11PluginABI.h` — `open`, `loop_fd`, `pump`, `close`, mirroring
CLAP's gui and posix-fd extensions) to a `dlopen`ed plugin copy that animates a
`GPUView` from a 60 Hz `Timer` and reaches the host's loop only through that
descriptor. When the host executable is itself an eacp app, none of that is
needed: the copy running the process's root loop (`EventLoop::run`, or an
outermost `runFor`) advertises a bridge — `EACP_ROOT_LOOP=1` beside
`EACP_ROOT_LOOP_BRIDGE=<pid>:<address>`, the address of a static C table of
`attach(fd, pump, context)`, `detach(fd)` and `stop()` in the root copy, the
same environment channel `EACP_ROOT_LOOP_THREAD` is on Windows and so no
`-rdynamic` on any host — and a copy for which `Platform::isDLL()` is true
hands its epoll fd across it on the first thing that defers work
(`attachCurrentThreadAsMain`, the first `callAsync`, `addLoopSource`, a
`Window`, `Detail::runAsPlugin`). The root copy pumps that fd as a loop
source, both on readiness and as a `prepare` before it waits, so a hosted
`Window`, `Timer`, `callAsync` or `Apps::run<T>` app runs with neither side
knowing the other; `stopProcessRootLoop` is the table's `stop`, which is how
a hosted app's `Apps::quit()` ends the thin host. The hosted `LoopState`
detaches in its destructor, before its epoll fd closes and before `dlclose`
unmaps the image, and the root holds each guest's pump behind an atomic it
clears on detach, so a callback already copied out for the round calls into
nothing, so a bare `DynamicLibrary::close()` inside a loop round is as safe
as `Plugins::unload`'s deferred one. The pid
is part of the advertisement because a child process inherits the environment
without the address space, and a magic/size pair refuses a copy built from
another revision. Under a foreign host nothing advertises and the path is
inert. `Tests/Core/RootLoopTests-Linux.cpp` (10 `RootLoop/` cases over the
`RootLoopTestPlugin` fixture) covers it, and `Apps/Plugins/PluginHost` with
`DemoPlugin` — both now a `GPUView` (`Apps/Plugins/SpinningTriangle.h`), so
the pair builds wherever `EACP_HAS_GPU` does — is the demo: the plugin's
toplevel, its timer and its `callAsync` all run off the host's loop. `X11Host`
now takes both paths at once, since it is an eacp app as well: the plugin
attaches through the bridge and the host registers the same fd through the C
ABI, and `addLoopSource` keeps whichever came last.

Under both is the Vulkan backend (`GPU/Vulkan/`): everything from `Device` to
`RenderPass` is real, the drawable `Frame` presents a swapchain image, and
`GPUView-Linux.cpp` owns the swapchain over the view's subsurface or child
window (`eacp-vulkan` defines `VK_USE_PLATFORM_WAYLAND_KHR` and
`VK_USE_PLATFORM_XCB_KHR`, and the instance enables `VK_KHR_wayland_surface`
and `VK_KHR_xcb_surface` each when the driver offers it, so one binary presents
to either; mailbox or FIFO; frames in flight on the context timeline; rebuilt
on resize and `OUT_OF_DATE`; continuous mode paced by whatever answered
`requestFrameCallback` — the compositor on Wayland, the pacer on X11 — with
`setMaxFps` skipping early ticks rather than running a timer), every pipeline
built through one `VkPipelineCache` persisted under `$XDG_CACHE_HOME/eacp/`,
with the off-screen `renderNativeContent` path
unchanged beside it. The GPU module knows the window system only as the
`NativeSurfaceHandle` it branches on in `createSurface()` and neither links
nor includes it. Under `EACP_HEADLESS=1`, with neither `WAYLAND_DISPLAY` nor
`DISPLAY` to reach, or when the preferred backend cannot connect, a window is
built with no surface, exactly the headless backend this grew out of. Device loss is terminal (no `VkDevice`
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
`TextMetrics`, `TextInput`, the retained layer classes and the image codecs are
left out of the Linux source list rather than stubbed, and with them
`SVGBuilder`/`SVG::parse`, `Apps/Graphics`, `Apps/SVG`, `Apps/UI/SVGDocument`
and the `Apps/GPU` examples that paint a 2D overlay. `EACP_HAS_CONTEXT` is also a PUBLIC compile definition on
`eacp-graphics`, and the `Graphics.h` umbrella leaves those headers out
where it is 0. `Path` is there as recorded geometry only
(`Primitives/Path-Linux.h`). Every GPU test but the Metal-only
`TextureInteropTests.mm` runs there on lavapipe.

`-DEACP_BUILD_GRAPHICS=OFF` is the only way to build Linux without any of this;
there is no Linux-specific switch. The `Dockerfile` reproduces all three steps
of the CI Linux lanes:

```bash
docker run --rm -e EACP_HEADLESS=1 -e EACP_REQUIRE_GPU=1 -e EACP_VK_SOFTWARE=1 \
      -e EACP_REQUIRE_FONTS=1 -v "$PWD":/workspace eacp-ci-linux \
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

`EACP_VK_SOFTWARE=1` prefers a CPU device (Mesa's lavapipe), mirroring
`EACP_D3D12_WARP`; `EACP_REQUIRE_GPU=1` makes `GPUTests` fail rather than
self-skip when no device came up; `EACP_VK_VALIDATION=1` turns on
`VK_LAYER_KHRONOS_validation` with a debug-utils messenger that logs. The
second and third commands are how the window and present tests run for real,
one window system each: `Scripts/with-weston` (also `with-weston` in the image)
wraps a command in a headless Weston session, which is where
`WaylandWindowTests` and `GPUTests`' `Present` cases run, and
`Scripts/with-xvfb` (`with-xvfb`) wraps one in an Xvfb with no window manager
at all — the harshest thing a toplevel meets — exporting
`EACP_WINDOW_SYSTEM=x11` and `EACP_XVFB_OUTPUT`, which is where
`X11WindowTests` (47 cases over two sources, its own `main` defaulting the
same override so the binary run by hand on a desktop still tests X11; the 9
`X11/clipboard` ones in `X11ClipboardTests-Linux.cpp` drive a second xcb
connection as another client, on a thread of its own where it has to own the
selection while eacp blocks reading it), `EmbeddedViewTests` (17, the
same default) and the same `Present` cases run over `VK_KHR_xcb_surface`. Only
the X11 suites are filtered in and out; everything else already ran under
Weston. `EACP_REQUIRE_DISPLAY=1` makes those tests fail rather than self-skip
without a display server, as
`EACP_REQUIRE_FONTS=1` does for the font tests. Weston's headless backend has
no seat, so Xvfb is the first place input is exercised on any lane: 18 of the
`X11WindowTests` cases and 2 of the embedded ones drive the server's own
pointer and keyboard through XTest, and under XWayland — where the compositor
owns the seat, so neither XTest nor a warp
reaches anything — they self-skip again. Two more need no display at all: a
scroll valuator and an emulated wheel button are arithmetic, and neither server
this suite can run on has a device with a scroll class to drive one for real.
Five more rewrite the root's
`RESOURCE_MANAGER` to drive the scale and the cursor theme, and one drives
RandR, all keyed on `EACP_XVFB_OUTPUT`: a desktop's resource database and
screen size belong to the desktop, so on a real session they skip and the rest
of the suite runs at whatever scale that session named. A display has one
pointer, one
keyboard, one focus and one clipboard, and with no window manager under Xvfb
every window a case opens lands on top of the last one and exposes it into a
repaint, so `Tests/Graphics/CMakeLists.txt` and `Tests/GPU/CMakeLists.txt` read
the case names back out of the sources (which are `CMAKE_CONFIGURE_DEPENDS`, so
a renamed case is not silently left unlocked) and give them a `RESOURCE_LOCK`:
`eacp-linux-display` over every `X11/`, `EmbeddedView/` and `Present/` case, and
`eacp-system-clipboard` over the clipboard ones, which take both. Each
serialises its own set under `ctest -j` while the rest of the suite runs beside
them.
`DisplayLinkTests` is a fourth Linux-only binary and a headless one: it plays
host to the loop, as `EmbeddedViewTests` does, so
`DisplayLink/ticksOnlyWhenPumped` needs a plain `main` outside `Apps::run`,
which `pumpEventLoop`'s refusal to re-enter would otherwise make a no-op.
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
  events another reader left queued. The loop is one `epoll` instance holding
  the waker and every source, so the whole copy is a single descriptor
- `getEventLoopFd()` / `pumpEventLoop()` (`Threads/EventLoop-Linux.h`, Linux
  only): the hosted loop. A plugin host that owns the process's loop watches
  the one fd (VST3 `IRunLoop`, CLAP posix-fd) or calls the pump from a timer
  or idle callback (CLAP timer-support, LV2 `idle`); one pump runs every
  ready source, every pending `callAsync`/`callAfter`/`Timer`/`DisplayLink`/
  `Async` delivery and then every `prepare`, never blocks, and is a no-op when
  re-entered. `run()`/`runFor()` are the same pump behind a `poll()` on the
  fd, so the standalone loop exercises the hosted one on every tick.
  `attachCurrentThreadAsMain` is real on Linux (not the `EventLoop-Default`
  no-op): it makes `isEventLoopRunning` true so deferred work is kept for a
  pump rather than dropped. Under an eacp host no host code is needed at
  all: the root copy advertises a bridge in the environment and a
  `Platform::isDLL()` copy attaches its fd to it on first use, so a hosted
  copy's callbacks, timers and windows run off the host's loop and
  `stopProcessRootLoop` is real (see "The Linux Backend"). No plugin SDK
  enters eacp; the VST3/CLAP/LV2 wrapper lives in the plugin project
  (`plan.md` D1, D9, D10)

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

### CPU compute (`Lib/eacp/GPU/CpuCompute`)

`eacp-cpu-compute` (namespace `eacp::GPU::CpuCompute`, force-optimised in
every configuration) interprets the `ShaderGraph` a compute kernel records, on
the calling thread over plain arrays, linking `eacp-gpu-codegen` alone. It rests
on `ComputeKernel` (`Codegen/ComputeKernel.h`), the device-free half that
`ComputeProgram` derives from, so every existing kernel runs unchanged and a
CPU-only one derives from `ComputeKernel`. `Bindings` is a fixed table of
`ComputePass::maxBufferSlots` spans set through the kernel's own members;
`Executor` is a `Plan` plus a `Workspace`, allocated and validated at
construction (`isValid()`/`reason()`), with `ComputePass`'s dispatch forms and
`prepareDispatch`/`dispatchGroups` for the caller's threads. No dispatch
allocates, locks, logs or makes a syscall. Semantics, the undefined-case table
and the realtime contract: `Lib/eacp/GPU/README.md`, "Running a kernel on the
CPU"; the GPU-against-CPU test helper is `Tests/GPU/CpuCrossCheck.h`.

### Key Design Patterns

- **Pimpl**: Platform-specific implementations hidden behind abstract interfaces
- **Template Factory**: `run<T>()` creates applications from user-defined structs
- **RAII**: Automatic resource cleanup via C++ destructors, especially for ObjC/CF objects
- **View Hierarchy**: Composable UI through `addSubview()`/`removeSubview()`

### Framework Dependencies

macOS: Foundation, Cocoa, CoreVideo, CoreGraphics, CoreText, Metal.
Windows: Direct2D, DirectWrite, D3D11/D3D12, DXGI, DirectComposition, WinHTTP.
Linux: pthreads, libcurl, wayland-client, wayland-cursor, xkbcommon, libdecor,
xcb with xcb-xkb, xkbcommon-x11, xcb-randr, xcb-xfixes, xcb-cursor,
xcb-icccm and xcb-xinput, FreeType, HarfBuzz and fontconfig, plus the Vulkan
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