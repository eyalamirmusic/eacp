# Linux X11 support and the hosted event loop — plan

Written 2026-09-07 against `bb1a6c8` (branch `linux-x11`), from a read of the
Wayland backend, the Linux event loop, the Vulkan swapchain path, the
Windows/macOS `EmbeddedView`s and the plugin-hosting paths in `Core`. Line
counts are estimates, not commitments.

## Progress

| Stage | State | Notes |
| --- | --- | --- |
| 0 — hosted event loop | **done** 2026-09-07 | `HostedLoopTests-Linux.cpp`, 16 cases; Core suite 212/212 |
| 1 — carve the seam | **done** 2026-09-07 | full headless suite 1568/1568, Wayland/Present on Mutter 28/28 |
| 2 — X11 toplevel | **done** 2026-09-08 | `X11WindowTests` 27 cases (13 of them input, on Xvfb only); full suite 1596/1596; `with-xvfb` X11+Present 36/36 ×5; XWayland and Mutter green |
| 3 — EmbeddedView | **done** 2026-09-15 | `EmbeddedViewTests` 17 cases (2 of them seat-driven, on Xvfb only), plus `Present/anEmbeddedViewPresentsIntoItsHost`; headless suite 1670/1670; `with-xvfb` X11+EmbeddedView+Present 54/54 ×6; XWayland green |
| 4 — in-tree fake host | **done** 2026-09-15 | `X11Host`/`X11Plugin` over a four-function C ABI: 346 frames in 6 s on bare Xvfb over lavapipe, 290 in 5 s under XWayland on a real GPU |
| 5 — parity and polish | **done** 2026-09-16 | clipboard, `Xft.dpi`, RandR change events, the cursor-theme rebuild and XI2 (raw motion for the lock, scroll valuators): `X11WindowTests` 47 cases (9 clipboard, 6 scale/RandR/cursor, 5 XI2); `with-xvfb` X11+EmbeddedView+Present 74/74; headless 1616/1616; XWayland at scale 2 64/64. `Present` pacing left alone: nobody has seen the pacer misbehave on hardware |
| 6 — thin eacp host bridge | **done** 2026-09-17 | `RootLoopTests-Linux.cpp`, 10 cases over a fixture plugin; Core suite 251/251, headless 1857/1857, `with-xvfb` X11+EmbeddedView+Present 74/74, `with-weston` Wayland+Present 23/23; `PluginHost`/`DemoPlugin` on `GPUView`, building and running on Linux (plugin timer, `callAsync` and 9 presented frames off the host's loop under Xvfb and Weston); X11Host 170 frames in 3 s |

*2026-09-17: `origin/develop` (8301b53b) merged in — after it the headless
suite is 1847/1847, `with-xvfb` X11+EmbeddedView+Present 74/74 and
`with-weston` Wayland+Present 23/23, so the merge costs the backend nothing.*

Where the code differs from the sketches below, the code wins; each such
point is marked *as built* in place.

## 0. Why

Audio plugins on Linux are embedded into the host's window, and every plugin
API only knows how to say that in X11:

| API | Parent handed to the plugin | How the host lets the plugin run |
| --- | --- | --- |
| VST3 | `IPlugView::attached(void*, "X11EmbedWindowID")`, an X11 `Window` id | `Linux::IRunLoop` off `IPlugFrame`: `registerEventHandler(handler, fd)` and `registerTimer(handler, ms)`, both delivered on the host's UI thread |
| CLAP | `clap_window_t` with api `"x11"`, `.x11` an `unsigned long` id | `clap.posix-fd-support` (`register_fd` → `on_fd`) and `clap.timer-support` (`register_timer` → `on_timer`) |
| LV2 | `ui:X11UI`, parent via the `ui:parent` feature, an id | `ui:idleInterface`: the host calls `idle()` at its own rate, nothing else |

Even on a Wayland desktop the DAW is an X11 client under XWayland and hands
out X11 ids. So a plugin built on eacp needs an X11 backend, an
`EmbeddedView` over an X11 id, and a message loop that the host drives rather
than one eacp runs — the fd, the timer or the idle call above. None of the
three exists on Linux today.

Standalone apps keep Wayland as the first choice. X11 is also what a
standalone app falls back to on a session with no `WAYLAND_DISPLAY`, which is
a second, smaller reason to have it.

## 1. What is there and what is missing

**The loop** (`Core/Threads/EventLoop-Linux.cpp`). A per-copy `poll()` loop
over a pipe waker and the `addLoopSource` set, with `prepare` callbacks run
before every wait. It only ever runs inside `run()`/`runFor()`: nothing pumps
it in a process whose loop belongs to someone else. In a plugin `.so`,
`Apps::run<T>` sees `Platform::isDLL()` and goes through `runAsPlugin` →
`scheduleStartup` → `callAsync`, which on Linux sits in the queue forever;
`Timer`, `DisplayLink`, `callAfter` and `Async` all post through the same
queue and are equally stranded; `attachCurrentThreadAsMain` is the
`EventLoop-Default.cpp` no-op, and the comment on it in `EventLoop.h` ("a
no-op where the main run loop is a process singleton (macOS/Linux)") is
wrong for Linux, where there is no process singleton at all. `isEventLoopRunning`
is false in a host, so anything that consults it before deferring drops the work.
Windows solved the same problem with a message-only window that the host's
pump dispatches into; macOS has the shared `NSApp`. Linux needs an explicit
pump the host can be handed.

**The window system** (`Graphics/Window/Wayland*-Linux.*`,
`Window-Linux.cpp`, `View-Linux.cpp`, `GPU/View/GPUView-Linux.cpp`). All
Wayland, and Wayland by name throughout: `Window::Native` derives from
`WaylandWindowSurface`; `ViewSurface` in `View-Linux.h` carries a
`wl_display*`/`wl_surface*` pair; `GPUView-Linux.cpp` calls
`vkCreateWaylandSurfaceKHR` on them; `Display-Linux.cpp`,
`DisplayLink-Linux.cpp` and `Keyboard-Linux.cpp` reach for `waylandDisplay()`
directly; the clipboard is a `WaylandClipboard`. Eleven files couple to the
Wayland headers. What is already backend-neutral, and worth keeping exactly
as it is: the view tree, hit-testing and `dispatchMouseEvent`, the
`ViewSurface` hook contract (`onAvailable`/`onLost`/`onResized`/`onRepaint`/
`onFrameDone`/`requestFrameCallback`), the per-view record bookkeeping in
`View-Linux.cpp` (sync on bounds, visibility, add/remove, deferred repaint,
origin-in-window), the whole of the Vulkan swapchain code below
`createSurface()`, the evdev→`KeyCode` table, the `Clipboard::Backend` hook
in `Core/App/Clipboard-Linux.h` (whose comment already anticipates X11), and
`Window`'s option handling.

**EmbeddedView** exists for macOS (`NSView` container) and Windows (child
`HWND`, calls `attachCurrentThreadAsMain` in its constructor) with the right
API for a plugin already: `setBounds`, `setSize`, `setVisible`,
`setPixelsPerPoint` (the host tells us the scale — exactly what
`setContentScaleFactor`/`set_scale` hand over, and what X11 cannot answer for
itself). It is gated by `EACP_HAS_CONTEXT`, which Linux lacks. That gate is
an accident of history: embedding is a windowing feature, not a 2D-drawing
one, and on Linux the content of an embedded surface is a `GPUView` tree.
[Stage 3 moved it to `EACP_HAS_DRAW`; the rest of this section is the
snapshot it was written as.]

**Vulkan.** `FindVulkanBackend.cmake` defines `VK_USE_PLATFORM_WAYLAND_KHR`
only; the instance enables `VK_KHR_wayland_surface` when offered. The
Vulkan-Headers checkout already has `VK_KHR_xcb_surface`; `chooseExtent`
already handles the X11 case (a real `currentExtent`, not 0xFFFFFFFF).

**Tests and CI.** `WaylandWindowTests` runs on a real compositor through
`Scripts/with-weston`; `GraphicsTests` runs headless. No X server is
installed on any CI lane. The dev VM has since gained every package D8
names (`xcb` 1.17, `xcb-xkb`, `xkbcommon-x11` 1.13, `xcb-randr`,
`xcb-xfixes`, `xcb-cursor` 0.1.6, `xcb-icccm` 0.4.2), `Xvfb`/`xvfb-run` and
`Xwayland`.

## 2. Decisions

**D1 — The hosted loop is one file descriptor plus one pump.** The Linux
`LoopState` moves from a rebuilt `pollfd` array onto an `epoll` instance
holding the waker and every `addLoopSource` fd. Two new entry points in
`EventLoop-Linux.h`:

```cpp
// The one descriptor a host loop watches for this eacp copy. Readable
// whenever pumpEventLoop() has something to do. Stable for the copy's life.
int getEventLoopFd();

// Runs every prepare, every ready source and every pending callback, and
// returns. Never blocks; safe to call when nothing is ready (LV2's idle).
void pumpEventLoop();
```

`run()` becomes `poll({epollfd})` + `pumpEventLoop()` in a loop and `runFor`
the same with a deadline, so there is one implementation and the standalone
loop exercises the hosted one on every tick. Sources come and go behind the
epoll fd, so a host registers one descriptor once; a Wayland or X11
connection opened after registration joins it with no round trip to the
host. `attachCurrentThreadAsMain` gets a Linux implementation: `initMainThread`,
create the loop state, set a `hosted` flag that makes `isEventLoopRunning`
true (work handed to `callAsync` will reach a pump) — the same promise
Windows makes through its message window. `EmbeddedView`'s constructor calls
it, as on Windows. `pumpEventLoop` refuses re-entry (a host may call `on_fd`
from inside a nested loop of its own; the inner call returns and the outer
one finishes the round). The `prepare` step must run at the end of a pump as
well as before a wait — in a host there is no wait, and the Wayland/X11
flush would otherwise not happen until the next readiness.

The three host shapes then map onto the same two calls, in the plugin
wrapper (which stays outside eacp, see D9):

| Host offers | Plugin does |
| --- | --- |
| fd readiness (VST3 `IRunLoop`, CLAP posix-fd) | `registerEventHandler(getEventLoopFd())`; `onFDIsSet` → `pumpEventLoop()` |
| timers only (CLAP timer-support, VST3 without fd) | a host timer at ~60 Hz → `pumpEventLoop()` |
| idle only (LV2) | `idle()` → `pumpEventLoop()` |

`Timer`, `DisplayLink` and `callAfter` keep their threads and post through
the waker, so under an fd host a continuous `GPUView` animates at display
rate, and under an idle-only host at the host's idle rate. Both are the
right answer for what the host offered.

*As built (stage 0).* One round is waker drain → `epoll_wait(0)` and dispatch
by descriptor (so a source removed earlier in the round is not found; up to
32 ready per round, level-triggered, the rest next round) → pending
callbacks → every `prepare`. Re-entry is a depth counter held for the whole
of `run`/`runFor` as well as each `pumpEventLoop`, so a host re-entering the
pump from a nested loop of its own is a no-op while `runEventLoopFor` from
inside a callback still pumps (`EventLoopSource/callbackCanPumpTheLoop`
depends on it). `run`/`runFor` dispatch once before their first wait, so
`runFor(0)` delivers a round before reporting a timeout. `stopEventLoop`
moved into `EventLoop-Linux.cpp` and `EventLoop-Default.cpp` left the Linux
source list. `stopProcessRootLoop` stays a no-op until D10. Along the way
`Platform::isDLL()` was found true for any binary launched by a relative
path (`dladdr`'s `dli_fname` compared verbatim against `/proc/self/exe`);
`getCurrentModulePath` now canonicalises, since D3's rule reads it.

**D2 — xcb, not Xlib.** One connection per eacp copy, an fd for the loop,
thread-safe, no global error handler that exits the process, and it is what
`xkbcommon-x11`, `xcb-cursor` and Mesa's Vulkan WSI are written against. No
Xlib symbol anywhere in the tree.

**D3 — Both backends compiled in, chosen at runtime per window kind.** An
`EmbeddedView` is always X11: the id it was given is one. A toplevel
`Window` picks Wayland when `WAYLAND_DISPLAY` reaches a compositor and X11
otherwise, with `EACP_WINDOW_SYSTEM=wayland|x11` overriding for tests and
for users. Both connections may be live in one copy (a hosted copy that also
opens a toplevel; a test that does both), each pumped by its own loop
source. Process-wide questions with no window to hang off — `primaryDisplay`,
the `DisplayLink` period, the clipboard backend — ask the *preferred*
backend, decided once per copy by the same rule (`EACP_WINDOW_SYSTEM`, else
`Platform::isDLL()` → X11, else Wayland if reachable, else X11), so a plugin
never opens Wayland just to read a refresh rate. Headless stays what it is:
`EACP_HEADLESS=1`, or neither display variable set, gives surfaceless
windows.

*As built (stage 2).* `Window-Linux.cpp` is option handling plus one
`makeWindowNative()` switch over `linuxPreferredWindowSystem()`; the natives
are `LinuxWindowNative` implementations (`Window/LinuxWindowNative-Linux.{h,cpp}`:
the interface, and `LinuxWindowState`, the option-derived state and the
behaviour no window system decides — constraints, `resizeTo`, activation,
`closeRequested`), one each in `Window/WaylandWindow-Linux.cpp`,
`Window/X11Window-Linux.cpp` and `Window/HeadlessWindow-Linux.cpp`. A native
is `LinuxWindowNative` *and* its window system's surface (`WaylandWindowSurface`
or `X11WindowSurface`, both `LinuxWindowSurface`), so the interface does not
derive from the surface. The headless native is explicit now, with a
`ViewSurfaceBackend` that makes no surfaces, and is what `EACP_HEADLESS=1`, an
unreachable server and a preferred backend that cannot connect all get.
`x11Connection()` is not gated on the preference (stage 3's `EmbeddedView` is
X11 under any preference), only the toplevel choice and the process-wide
questions are; `linuxSeatInput()` is, so a session with both variables set
never opens the other connection to answer a pointer question.

**D4 — The seam is `ViewSurface`, made backend-neutral.** The
`wl_display*`/`wl_surface*` pair in `View-Linux.h` becomes a small tagged
handle:

```cpp
struct NativeSurfaceHandle
{
    enum class Kind { None, Wayland, X11 } kind = Kind::None;
    void* connection = nullptr;   // wl_display* or xcb_connection_t*
    void* surface = nullptr;      // wl_surface*
    uint32_t window = 0;          // xcb_window_t
};
```

`GPUView-Linux.cpp` gains one branch, in `createSurface()` only:
`vkCreateWaylandSurfaceKHR` or `vkCreateXcbSurfaceKHR`. Every hook and every
line below stays. The record bookkeeping in `View-Linux.cpp` becomes the
portable half, calling a per-window `ViewSurfaceBackend` for the three
things that differ — create a native child at the view's bounds, destroy it,
apply geometry and report whether the pixel size or scale changed — plus
`requestFrame`. Wayland's implementation is the current subsurface code;
X11's is an `xcb_create_window` child of the toplevel (or of the embedded
child), moved and sized with `xcb_configure_window`. The GPU module goes on
knowing the window system as opaque pointers and linking none of it.

*As built (stage 1).* `NativeSurfaceHandle` is as above plus `isValid()`.
The backend is two classes in `View/ViewSurfaceBackend-Linux.h`: a
per-window `ViewSurfaceBackend` with one virtual,
`createSurface(View&, ViewSurface&)` returning a
`std::unique_ptr<ViewSurfaceNative>`, and the per-view `ViewSurfaceNative`
with `applyGeometry()` (true when pixel size or scale changed) and
`requestFrame()`. Destroying the native is the destroy step, after
`View-Linux.cpp` has fired `onLost`, so there is no fourth virtual and no
second per-view map. `LinuxWindowSurface` (`Window/LinuxWindowSurface-Linux.h`)
owns the window's backend as `viewSurfaces`, and the same header declares the
four `linux*` functions `Window-Linux.cpp` needs of `View-Linux.cpp`. The
seat questions a view asks (`linuxPointerWindow`, `linuxPointerPosition`,
`linuxRefreshCursor`) live on `LinuxWindowSystem`. `Window::Native` is still
`WaylandWindowSurface` in one file; a `LinuxWindowNative` interface for two
natives is stage 2's, when the second one exists. `Keyboard-Linux.cpp` still
reaches `waylandDisplay()->getInput()` for polled key state — the neutral
seat interface it wants is also stage 2's, motivated by `X11Input`.

**D5 — Input is split into what the seat/server says and what eacp makes of
it.** `WaylandInput` holds, tangled together, protocol glue and a state
machine that is not Wayland at all: xkbcommon keymap/state/plain-state and
the UTF-8 for a key, key repeat, click counting and double-click slop, the
held button and drag origin, wheel accumulation, cursor shape. The state
machine moves to `Graphics/Window/LinuxInput-Linux.{h,cpp}` (working names:
`XkbKeyboardState`, `PointerTracker`, `WheelTracker`), consumed by
`WaylandInput` and a new `X11Input`. Keycodes: X11 keycodes are evdev + 8 on
every evdev/libinput X server and on XWayland, so `waylandKeyCodeFromEvdev`
serves both after a subtraction and the table is not duplicated (rename the
pair `linuxKeyCodeFromEvdev`/`linuxEvdevFromKeyCode`). The xkb keymap on X11
comes from the server through `xkb_x11_keymap_new_from_device`, so layouts
and dead keys behave identically on the two backends.

*As built (stage 1).* `LinuxInput-Linux.{h,cpp}` holds `XkbKeyboardState`,
`KeyRepeat`, `PointerTracker`, `WheelTracker`, `CursorTracker`,
`linuxTimestamp`, `linuxButtonFromEvdev` and `linuxCursorNames`. The pointer
serials and `wheelTime` stayed in `WaylandInput`: protocol values, not state.

*As built (stage 2).* `Window/X11Input-Linux.{h,cpp}` is the second
consumer, owned by the connection, and `Window/LinuxSeat-Linux.h` the seat
interface both it and `WaylandInput` implement (polled key state, modifiers,
`characterForCode`, keyboard focus, the pointer's window and position,
`refreshCursor`); `linuxSeat()` returns the preferred backend's, and
`Keyboard-Linux.cpp` and the pointer questions in `LinuxWindowSystem` go
through it with no Wayland header. The toplevel selects the input masks and
its `handleEvent` forwards key, button, motion, enter, leave and focus events
to `X11Input`; view children select `EXPOSURE` only, so everything arrives
toplevel-relative. The keymap is `xkb_x11_keymap_new_from_device`, refreshed
on XKB new-keyboard and map notifications, with modifier state from XKB
state notifications rather than the core event's `state`. Repeat is the
server's own: detectable auto-repeat is requested so the synthetic release
disappears, and a press of a key already in the pressed set is delivered
with `isRepeat`; `KeyRepeat` is not run on X11, since the server keeps
sending repeat presses whatever a client asks and the two would double up.
Wheel is buttons 4–7 as notches, dispatched per press (no frames on X11).
Cursors load through `xcb_cursor_load_cursor` over `linuxCursorNames`, cached
per shape, and are set on the toplevel. Mouse lock engages only with keyboard
focus on a mapped window: `xcb_grab_pointer` confined to the window,
`xcb_xfixes_hide_cursor`, a warp to the centre, and every motion after it a
delta from the centre followed by another warp (the warp's own motion, at the
centre exactly, is dropped). Focus-on-click (D6) is already in: a button press
in a mapped window without focus sends `xcb_set_input_focus`, harmless under
a window manager and the whole mechanism under none. Under XWayland the
compositor owns the seat, so XTest and warps reach no client and the 13 seat
cases self-skip there; on Xvfb they run, which makes the CI Xvfb step the
first lane on which input is exercised at all.

**D6 — `EmbeddedView` moves from `EACP_HAS_CONTEXT` to `EACP_HAS_DRAW`.**
The umbrella stops guarding it, `EmbeddedView-Linux.cpp` implements it as an
X11 child of the host's id (an `unsigned long` cast to the existing `void*`
parameter; documented in the header), and `Apps/Plugins` gets a Linux-buildable
half (stage 4). Behaviour follows the header's contract: fills the parent and
tracks its `ConfigureNotify` until `setBounds` is first called, then stays
put; `setPixelsPerPoint` is the scale (X11 has no per-window one); `setVisible`
maps/unmaps. Keyboard focus is the one X11-specific rule: hosts keep focus
on their own window and forward nothing, so the embedded child takes focus
with `xcb_set_input_focus(RevertToParent)` on a button press inside it and
gives nothing back — the same thing JUCE and iPlug do, and the reason typing
into a plugin works in one DAW and not another is the DAW, not the plugin.
No XEmbed protocol: none of the three hosts speaks it.

*As built (stage 3).* `Window/EmbeddedView-Linux.cpp` is `EmbeddedView::Native`
and an `X11WindowSurface`, and nothing else — no `LinuxWindowNative`, no
`WindowOptions`, no `WindowEvents`: there is no `Window` above it, and what a
toplevel decides for itself the host decides here. The child is an
`xcb_create_window` InputOutput window of the host's id, `XCB_COPY_FROM_PARENT`
in depth and visual because those are the only ones a child of it may have,
with no background pixmap so what the host painted stands until our own content
does, and the same event mask a toplevel selects — which moved to
`X11Connection-Linux.h` as `x11WindowEventMask` so both take it from one place.
Being an `X11WindowSurface` is the whole of the integration: the base
constructor installs the X11 `ViewSurfaceBackend`, so a `GPUView` inside
presents through stage 2's child-window path with nothing in `eacp-gpu` changed,
and the seat reaches the content view through the same `X11Input`.
`Threads::attachCurrentThreadAsMain()` is the first thing the constructor does,
as on Windows, before anything can defer work. The host's window is watched,
never owned: `StructureNotify` is selected on it with
`xcb_change_window_attributes` — skipped where the host is a window of this
copy's, whose own mask that would replace — and the id goes into a second list
on the connection (`watchForeignWindow`/`unwatchForeignWindow`, dispatched after
the window's own target and over a copy of the list, since a watcher may take
itself out from inside its handler), because registering a foreign id in the
window map would take that window's routing away from it. The parent's
`ConfigureNotify` sizes the surface until `setBounds` is first called and never
after, with `xcb_get_geometry` on the parent at construction as the starting
size and the options' size the fallback; a `ReparentNotify` moves the watch to
the new parent, and a size the host configures onto the child itself is adopted
whether or not anything is being followed. Only the child's own `DestroyNotify`
is acted on: the server hands a departed client's resource-id base straight back
out, so a stale notice for a previous host would otherwise kill a live surface
(a real failure while the tests were being written). `setPixelsPerPoint` is the
whole of the scale — 0 means 1, X11 having no per-window one to read — and
re-derives the content's point size and, for a placed surface, the pixels it
covers, rounded outwards as on Windows so no seam of the host's window is left
showing; `X11Input` now divides an event position by the window's scale
(identity on a toplevel) and multiplies a lock warp back up. Keyboard focus is
stage 2's focus-on-click unchanged. Connection loss and the child's
`DestroyNotify` both end in `markWindowGone`, which is also the surfaceless
object a null `x11Connection()`, `EACP_HEADLESS=1` or a null host id give from
the start. `Graphics.h` moved `EmbeddedView` out of its `EACP_HAS_CONTEXT`
block.

**D7 — X11 frame pacing is a pacer, not a compositor signal.** X11 has no
`wl_surface.frame`. The X11 `ViewSurfaceBackend::requestFrame` arms a
one-shot on a per-connection pacer running at the RandR mode's rate (the
same clock `DisplayLink-Linux.cpp` already keeps) and fires `onFrameDone`
from it; `frameCallbackPending` keeps its meaning. FIFO presentation
throttles the swapchain to the server anyway. The Present extension's
`PresentCompleteNotify` is the true equivalent and can replace the pacer
later without touching `GPUView`; it is not in scope.

*As built (stage 2).* `View/X11ViewSurface-Linux.cpp`: a view's native is
an `xcb_create_window` child of the toplevel selecting `EXPOSURE` only, so
pointer and key events propagate to the toplevel with toplevel-relative
coordinates and `linuxViewOriginInWindow` is the only subtraction a hit test
needs; it is registered in the connection's map with the toplevel as its
`windowSurface` and the view beside it, so a child's `Expose` reaches the
toplevel's `handleEvent` and is routed back to the view's `repaint()`. The
pacer is one process-wide `X11FramePacer`, leaked like the connection: a
`Threads::Timer` made on the first `requestFrame` at the RandR mode's rate
(24–480 Hz, else 60 — Xvfb reports no mode) that moves the armed set into a
batch per tick, clears `frameCallbackPending` and fires `onFrameDone` per
record; a re-arm from inside lands in the next batch, a native destroyed
while armed is nulled out of the in-flight batch, and the timer is dropped as
soon as nothing is armed — synchronously from `disarm` when the last presenting
view goes, through a `callAsync` only when it is a tick itself that ends with
nothing armed — so an idle connection runs no thread and no pacing thread
outlives the last presenting view. Stage 3 needed the synchronous half: a
plugin's `close()` is followed by `dlclose` with no pump in between, and a
`Threads::Timer` still ticking into an unmapped image is the classic plugin
crash. The same stage added `X11WindowSurface::inferiorsGone`, set from a
`DestroyNotify` the server sent rather than one we asked for (a host taking its
own window down, a `KillClient`), so a view's child window is unregistered
without a `DestroyWindow` for what the server already reaped — the toplevel had
the same latent `BadWindow`.

**D8 — X11 is a mandatory Linux dependency, like Wayland.** One
`CMake/FindX11Backend.cmake` producing `eacp-x11` (pkg-config: `xcb`,
`xcb-xkb`, `xkbcommon-x11`, `xcb-randr`, `xcb-xfixes`, `xcb-cursor`,
`xcb-icccm`), linked PRIVATE into `eacp-graphics` beside `eacp-wayland`.
Optional backends would double the configuration matrix for a set of
headers every distribution ships. `FindVulkanBackend.cmake` adds
`VK_USE_PLATFORM_XCB_KHR` (PUBLIC, for `volk.c`), and the instance enables
`VK_KHR_xcb_surface` when offered, independently of the Wayland one.

*As built (stage 2).* `CMake/FindX11Backend.cmake` → `eacp-x11`, an
INTERFACE target over the seven pkg-config modules, PRIVATE on
`eacp-graphics`; `eacp-vulkan` defines both `VK_USE_PLATFORM_*_KHR` and
carries xcb's include directory for `vulkan_xcb.h`; the instance enables
`VK_KHR_wayland_surface` and `VK_KHR_xcb_surface` each when offered, and
`GPUView-Linux.cpp` switches on `NativeSurfaceHandle::Kind` with each
creator null-guarded (volk leaves the pointer null when the extension was not
enabled). lavapipe presents to Xvfb with no help: every `Present` case passes
there. `X11Connection-Linux.{h,cpp}` is the twin of `WaylandDisplay`: xcb
connection, the atoms interned in one batch, XKB (`xkb_x11_setup_xkb_extension`,
core device id, event base — `xcb/xkb.h` names a field `explicit` and is
wrapped where it is included), RandR 1.3 negotiated for the primary output
(root size and no refresh where there is none, as on Xvfb), XFixes and a
`xcb-cursor` context, the id-to-target map, and the loop source whose prepare
drains `xcb_poll_for_queued_event` before `xcb_flush` — the events Mesa's WSI
pulled off the socket. Connection loss is `xcb_connection_has_error` after
either step, fired once, and `xcb_disconnect` waits for the destructor.

**D9 — No plugin SDK enters eacp.** eacp's plugin story on Linux is the
four calls above (`EmbeddedView`, `setPixelsPerPoint`, `getEventLoopFd`,
`pumpEventLoop`) plus `attachCurrentThreadAsMain`. A VST3/CLAP/LV2 wrapper
lives in the plugin project. The demo in stage 4 stands in for one with a
four-function C ABI so the whole path is exercised in-tree.

*As built (stage 4).* About 330 lines under `Apps/Plugins`, and nothing in
`Lib`: `X11WindowNative::getHandle()` already hands the toplevel's
`xcb_window_t` back through `Window::getHandle()`, which is the whole of what a
host has to give. `Apps/CMakeLists.txt` enters `Plugins` under
`EACP_HAS_CONTEXT OR LINUX` and `Apps/Plugins/CMakeLists.txt` splits itself in
two — `DemoPlugin`/`PluginHost` on the 2D tier, `X11Plugin`/`X11Host` on
`LINUX AND EACP_HAS_GPU`. The four are `eacp_x11_plugin_{open,loop_fd,pump,close}`,
declared once in `Apps/Plugins/X11PluginABI.h` as function-pointer aliases
beside their symbol names, with the id an `unsigned long` exactly as
`clap_window_t::x11` and VST3's `"X11EmbedWindowID"` hand one out.
`EACP_PLUGIN_EXPORT` over the tree's global `-fvisibility=hidden` and
`CMAKE_POSITION_INDEPENDENT_CODE` was the whole of the isolation work — `nm -D`
shows those four `T` symbols and no other strong one — plus `--no-undefined` on
the plugin's own link, because a MODULE links with a missing platform source and
only fails at `dlopen`. There is no fifth function for resizing: the surface
follows the parent's `ConfigureNotify`, and `--exit-after-ms` drives an
unattended run that resizes the host window once to prove it. The host sets
`EACP_WINDOW_SYSTEM=x11` in `main`, before either copy opens a window and reads
the preference. Measured: 346 presented frames in 6 s on bare Xvfb over lavapipe
and 290 in 5 s under XWayland on a real GPU, from a 60 Hz `Timer` in the plugin
copy reaching the host only through `getEventLoopFd()`.

**D10 — The thin eacp host (`PluginHost` + `DemoPlugin`) is a separate,
optional bridge.** Under a foreign host the plugin registers its fd and
nothing crosses copies. Under an eacp host there is no host loop API to
register with, so a hosted copy would need to find the root copy's loop: a
default-visibility C symbol exported by `eacp-core`
(`eacp_root_loop_attach(int fd, void (*pump)(void*), void*)`), found with
`dlsym(RTLD_DEFAULT)`, through which the root copy adds the hosted copy's
epoll fd as a loop source; `stopProcessRootLoop` rides the same channel.
Worth doing for the Linux `Apps/Plugins` demo and for `Plugins::unload`'s
deferral, but nothing a DAW needs — so it is its own stage and can slip.

*As built (stage 6).* The channel is the environment rather than `dlsym`:
the root copy sets `EACP_ROOT_LOOP=1` (the marker macOS and Windows already
use) and `EACP_ROOT_LOOP_BRIDGE=<pid>:<address>`, the address of a
constant-initialised, trivially destructible `RootLoopBridge` in the root
copy — `magic`, `size`, `attach(fd, pump, context)`, `detach(fd)`, `stop()`,
function pointers and a `void*` and nothing with C++ layout — exactly the
shape `EACP_ROOT_LOOP_THREAD` takes on Windows. That needs no
`ENABLE_EXPORTS`/`-rdynamic` on any thin host (`X11Host` deliberately exports
nothing) and no interposition between two copies that both define the symbol.
The pid is in the advertisement because `Processes::Process` children inherit
the environment without the address space, so an inherited address would be a
wild pointer where the inherited marker was only a wrong answer — a child now
correctly answers `isEventLoopRunning()` false, where macOS and Windows still
say true — and `magic`/`size` refuse a copy built from another revision.
Both `run()` and an outermost `runFor()` advertise, nesting-aware through a
`rootLoopDepth` (Windows marks only `run()`, but CoreTests drive their loops
through `runFor`, and a nested `runEventLoopFor` inside a hosted copy never
takes the advertisement over because `publishRootLoop` refuses when another
copy already holds it). The hosted side attaches lazily and idempotently from
`attachCurrentThreadAsMain`, the first `EventLoop::call` on the main thread,
`addLoopSource`, `Detail::runAsPlugin` and the Linux `Window` constructor
(gated on `Platform::isDLL()`, unlike Windows, because on Linux
`attachCurrentThreadAsMain` sets `hosted` and a standalone app must not
answer `isEventLoopRunning` true through teardown). The root registers the
guest's pump as both the source callback and its `prepare`, so a hosted copy
that opened a display connection from a call the host made into it directly
still flushes before the root waits. Detach safety is a guard as well as a
removal: the root holds a `shared_ptr<RootLoopGuest>` per attached copy with
the pump behind an atomic, cleared on detach, so a callback already copied
out of the source list for the round calls into nothing; `~LoopState`
detaches before closing its epoll fd, and that destructor does run at
`dlclose` for a `MODULE` built by `eacp_add_plugin` — checked by commenting
the detach out, which segfaults both unload cases. Nothing else changed in
`Core`: `stopProcessRootLoop` is the table's `stop`. `X11Host` now takes
both paths at once, being an eacp app itself — the plugin attaches through
the bridge and the host registers the same fd through the C ABI, and
`addLoopSource` keeps whichever came last; `RootLoop/withNoRootLoopNothingAttaches`
is the pure foreign-host case. Not made airtight: `isEventLoopRunning()`
off the main thread reads the environment on its cold path, the pre-existing
macOS/Windows pattern, while glibc's `setenv` can reallocate `environ` under
a concurrent `getenv`; and if a hosted copy's `CallAfterScheduler` were ever
constructed before its `LoopState` it would be joined after the detach — no
path constructs that order, and 40 unload cycles under ASan saw nothing.

## 3. Files

Renamed or split (pure refactor, Wayland tests stay green):

- `Window/WaylandDisplay-Linux.h`: `WaylandWindowSurface` → `LinuxWindowSurface`
  in a new `Window/LinuxWindowSurface-Linux.h` (content view, content size,
  scale, mapped, focus and connection-lost callbacks, a `NativeSurfaceHandle`),
  with `WaylandWindowSurface` deriving from it and holding the `wl_surface*`.
- `View/View-Linux.h`: `ViewSurface::display/surface` → `NativeSurfaceHandle`;
  new `ViewSurfaceBackend` interface in `View/ViewSurfaceBackend-Linux.h`.
- `View/View-Linux.cpp`: keeps the records, sync and repaint logic; the
  create/destroy/geometry bodies move to `View/WaylandViewSurface-Linux.cpp`.
- `Window/WaylandInput-Linux.cpp` → protocol glue only; the state machines to
  `Window/LinuxInput-Linux.{h,cpp}`.
- `Graphics/Keyboard-Linux.{h,cpp}`: functions renamed `linux*`.
- New `Window/LinuxWindowSystem-Linux.{h,cpp}`: the preferred-backend rule,
  `primaryOutput()` (frame, scale, refresh) and clipboard installation, so
  `Display-Linux.cpp`, `DisplayLink-Linux.cpp` and the clipboard no longer
  name Wayland.

New:

- `Core/Threads/EventLoop-Linux.{h,cpp}`: epoll, `getEventLoopFd`,
  `pumpEventLoop`, Linux `attachCurrentThreadAsMain` (leaves
  `EventLoop-Default.cpp`).
- `CMake/FindX11Backend.cmake` → `eacp-x11`.
- `Window/X11Connection-Linux.{h,cpp}`: `xcb_connect`, atoms, screen, RandR
  primary output and refresh, the id→`LinuxWindowSurface` map, the loop
  source (`prepare` = drain `xcb_poll_for_queued_event` then `xcb_flush`;
  ready = `xcb_poll_for_event` loop and dispatch), `xcb_connection_has_error`
  → the same connection-lost path as Wayland.
- `Window/X11Window-Linux.cpp`: `Window::Native` for X11 — toplevel with
  `WM_PROTOCOLS`/`WM_DELETE_WINDOW`, `_NET_WM_NAME`, `WM_CLASS`,
  `WM_NORMAL_HINTS` (min size, fixed size when not resizable, aspect),
  `_MOTIF_WM_HINTS` for `Borderless`, `_NET_WM_STATE` for always-on-top,
  maximise and fullscreen, iconify, `ConfigureNotify` → resize/`onMoved`
  (positions are real here, unlike Wayland), `FocusIn/Out` → activation,
  `Expose` → repaint of presenting children. *As built:* the only state the
  window sends is a `_NET_WM_STATE` toggle of the two maximise atoms, with
  iconify going out as `WM_CHANGE_STATE`. `_NET_WM_STATE_ABOVE` and
  `_NET_WM_STATE_FULLSCREEN` are interned and never used, and neither Linux
  backend honours `WindowOptions::alwaysOnTop` or `allowsFullScreen`.
  `Window-Linux.cpp` keeps the option handling and constructs one of two
  natives.
- `Window/X11Input-Linux.cpp`: core pointer and key events into the shared
  state machines; XKB extension events for keymap and state changes; cursor
  through `xcb-cursor`; mouse lock as a pointer grab + hidden cursor + warp
  to centre, deltas from the warp (XI2 raw motion is a later refinement,
  as is XI2 smooth scrolling in place of buttons 4–7).
- `View/X11ViewSurface-Linux.cpp`: the child-window `ViewSurfaceBackend`
  and the pacer of D7.
- `Window/X11Clipboard-Linux.{h,cpp}`: `CLIPBOARD` selection owner and
  requestor, `UTF8_STRING` and `text/uri-list`, `TARGETS`; INCR for large
  transfers can wait.
- `Window/EmbeddedView-Linux.cpp`.
- `GPU/View/GPUView-Linux.cpp`: the `createSurface()` branch.
- `Scripts/with-xvfb`, and `Tests/Graphics/X11WindowTests-Linux.cpp`,
  `Tests/Graphics/EmbeddedViewTests-Linux.cpp`,
  `Tests/Core/HostedLoopTests-Linux.cpp`, `Tests/GPU/PresentTests-Linux.cpp`
  parameterised over both backends.

## 4. Stages

Each stage is one merge, green on all three Linux lanes and on macOS.

**Stage 0 — hosted event loop — done.** D1 in full, plus the `EventLoop.h` comment
fix. Tests (`HostedLoopTests-Linux.cpp`, headless, every lane): a fake host
loop that `poll()`s `getEventLoopFd()` and calls `pumpEventLoop()`; callAsync,
`callAfter`, `Timer`, `DisplayLink` and `Async` all deliver only when pumped;
`isEventLoopRunning` after attach; `runEventLoopFor`/`runEventLoopUntil`
still work standalone and from a hosted copy; a source added from a callback
is polled next round; re-entrant pump is a no-op; `addLoopSource` and
`removeLoopSource` tests keep passing. Also `PluginHost` on Linux is not in
scope here (D10). ~250 lines changed, ~200 of tests. *Landed as planned except that
`DisplayLink` is not covered by `HostedLoopTests`: it lives in
`eacp-graphics`, which `CoreTests` must not link. Its pacing thread posts
through `callAsync` exactly as `Timer` does, which is covered; a
"`DisplayLink` ticks only when pumped" case belongs in `Tests/Graphics` and
goes in with stage 2.*

**Stage 1 — carve the seam — done.** Everything under "renamed or split" in §3, D4
and D5, `LinuxWindowSystem` with only the Wayland backend behind it. No
behaviour change; `WaylandWindowTests`, `GraphicsTests`, `GPUTests` on
Weston prove it. ~600 lines moved, ~150 new. *Landed: about 1,050 lines new
and 830 gone; no file was a 1:1 rename, so nothing went through `git mv`.
Every suite matched its baseline exactly (`GraphicsTests` 124,
`WaylandWindowTests` 13, `GPUTests` 390, `GPUWidgetsTests` 57, `UITests`
180). With the X11 backend not yet present, `EACP_WINDOW_SYSTEM=x11` gives
surfaceless windows, exactly as no compositor does.*

**Stage 2 — X11 toplevel — done.** D2, D3, D7, D8: `eacp-x11`, the connection, the
window, input, view surfaces, the Vulkan branch, the runtime choice. Tests
on Xvfb through `Scripts/with-xvfb` (package `xvfb`; `xvfb-run` gives a
`DISPLAY` with no compositor, which is also the harshest case), with
`EACP_WINDOW_SYSTEM=x11` and `EACP_REQUIRE_DISPLAY=1`: map/unmap, resize
through the WM-less path, `onMoved`, key and pointer delivery through a
synthetic `xcb_send_event`/XTest (Weston's headless seat problem does not
exist here — XTest works on Xvfb, so input *is* exercised on this lane,
which today it is nowhere), `GPUView` presents on lavapipe over
`VK_KHR_xcb_surface`, connection loss (kill the Xvfb) tears down cleanly.
The third CI lane runs `with-weston` for Wayland and `with-xvfb` for X11 in
two steps. On the dev VM, X11 tests run under GNOME's XWayland with the same
override. ~1,800 lines, ~400 of tests. *Landed: about 3,900 lines new and 840
gone, in four passes — the seam (`LinuxWindowNative`, the Wayland native moved
into `WaylandWindow-Linux.cpp`, an explicit headless native), the connection
and window, input, and a review pass that fixed a use-after-free in both
backends' `setKeyboardFocus` (an `onActivationChanged` that closes the window;
`X11/aWindowClosedFromOnActivationChangedIsSafe` segfaults without it), a
`ConfigureNotify` ping-pong against a window manager that enforces its own
size, the synchronous `setPosition`/`onMoved` contract, and a blocking
translate round trip per configure. Tests are `X11WindowTests` (27 cases)
and `DisplayLinkTests` (the stage-0 debt), plus `PresentTests` made
backend-aware; the CI Vulkan lane runs `with-weston ctest -E '^X11/'` and then
`with-xvfb ctest -R '^(X11|Present)/'`, every window-opening case on one
`RESOURCE_LOCK eacp-linux-display` because with no compositor a window
mapped over another's presenting child sends `Expose` and so an honest extra
frame. Two things the plan did not foresee: under XWayland the compositor
owns the seat, so XTest and `WarpPointer` reach no client and the input
cases self-skip there (Xvfb, which is the CI target, runs them all — the
first lane on which input is exercised); and `KeyRepeat` is not run on X11,
since the server repeats for itself whatever a client asks and detectable
auto-repeat only removes the synthetic release. Not verified by anyone yet:
real mouse and keyboard input into an X11 window under XWayland on a desktop,
which no test can drive.*

**Stage 3 — EmbeddedView and the plugin path — done.** D6. Test: the test itself
plays host — opens its own xcb connection, creates a parent window, builds
an `EmbeddedView` on the id, pumps through `getEventLoopFd()` from its own
`poll()` (never `runEventLoop`), asserts the child's geometry follows
`setBounds`/`setSize`, that a `GPUView` inside presents, that
`setPixelsPerPoint` changes the content view's point size, that the click
takes focus and a key arrives. `Graphics.h` and README/CLAUDE.md move
`EmbeddedView` to the draw tier. ~400 lines, ~250 of tests. *Landed:
`Tests/Graphics/EmbeddedViewTests-Linux.cpp`, 17 cases in a binary of its own
with a plain `main` — the test creates the parent window on an xcb connection of
its own and drives eacp only through `getEventLoopFd()`/`pumpEventLoop()`, which
is also why no case can sit inside `Apps::run` — of which 2 drive the seat
through XTest and skip again under XWayland, and one,
`EmbeddedView/aSurfaceWithNoHostIdIsHeadlessAndSafe`, needs no server at all
and asserts the content view is still laid out at the surface's point size, the
one thing a surfaceless surface got wrong on the first pass. A review pass
added four more — a second `setContentView`, two surfaces on one host, the host
destroying its own window under two presenting views, and a `/proc/self/task`
count proving no pacer thread survives the last presenting view — and made the
cases that read geometry back wait for the host window to settle first, so they
hold under a window manager as well as under none.
`Present/anEmbeddedViewPresentsIntoItsHost` in `Tests/GPU` is the swapchain
half, hosted by a `Graphics::Window` of the same copy and skipped off the X11
lane. `EmbeddedView/` joins the `eacp-linux-display` `RESOURCE_LOCK` and both CI
filters — out under Weston, in under Xvfb. Run as one process the binary logs a
few harmless `X11: protocol error 3` lines from the two-client teardown race;
under ctest each case is its own process. Two desktop-only failures under
XWayland and mutter — `X11/windowComesUpAtItsConfiguredSize` and
`Present/renderToImageWorksWhilePresenting`, both asserting an exact size mutter
widens by a pixel — predate this stage, pass on Xvfb, and did not reproduce in
the final runs.*

**Stage 4 — in-tree fake host — done.** `Apps/Plugins/X11Host` (Linux only): a
standalone eacp app whose window is X11 by override, exposing its content
view's id to a `dlopen`ed `X11Plugin.so` through a C ABI of four functions —
`open(parent_id, scale)`, `loop_fd()`, `pump()`, `close()` — that mirrors CLAP
posix-fd/gui exactly, so the plugin's `Timer`-driven `GPUView` animates from
the host's loop source. Doubles as the manual test against a real DAW
(REAPER and Bitwig both run natively on Linux). ~300 lines. *Landed: about 330
lines, `Apps/Plugins/X11Host` and `Apps/Plugins/X11Plugin` over
`X11PluginABI.h`. The id handed over is the host toplevel's own rather than a
content view's — `Window::getHandle()` already returned it — and each copy
resolves its own eacp, the executable exporting nothing and `DynamicLibrary`
opening the module `RTLD_LOCAL`; the host takes the descriptor out of its loop
before `Plugins::unload` defers the close. Not
verified by anyone yet: a run under `VK_LAYER_KHRONOS_validation`, which is not
installed on the dev machine, and a real DAW.*

**Stage 5 — parity and polish — done.** Clipboard (D8's `X11Clipboard`), XI2 raw
motion for mouse lock and smooth scrolling, cursor themes, `Xft.dpi` as the
standalone scale source, RandR change events, `Present` pacing if the pacer
proves visibly worse on real hardware. Each item independent.
*Landed 2026-09-16, the clipboard: ~430 lines in
`Window/X11Clipboard-Linux.{h,cpp}` plus 40 in the connection. Owned by
`X11Connection` and installed through the same `Clipboard::Backend` hook the
Wayland twin uses, so it is live for a copy that prefers X11 and silent for one
that does not. The owner is a 1×1 `InputOutput` window made on first use and
never mapped: unlike Wayland, where `set_selection` wants the serial of an
input event on a focused surface of ours, taking `CLIPBOARD` here needs no
toplevel and no keyboard focus, which is the whole point for a plugin copy.
`xcb_set_selection_owner` is verified with `xcb_get_selection_owner` before a
copy reports success; a `SelectionRequest` is answered with `TARGETS`,
`UTF8_STRING`, `text/plain;charset=utf-8`, `text/plain`, `STRING` and `TEXT`
for text and `text/uri-list` for files, honouring a requestor that names no
property and refusing `MULTIPLE` and everything else with a `SelectionNotify`
naming none; `SelectionClear` drops the store. The uri-list itself moved to
`linuxUriList` in `LinuxWindowSystem`, so both backends emit the same bytes
from one implementation. A read is `xcb_convert_selection` into one property
on that window followed by `X11Connection::dispatchUntil` — the loop source's
own drain-poll-dispatch cycle run by hand for a bounded two seconds, so a
window whose `ConfigureNotify` lands beside the answer still gets it, which is
the thing a hand-rolled `xcb_wait_for_event` would have quietly broken. Our
own selection is answered from the store with no round trip, since the
conversion would otherwise be served by the very thread waiting on it.
`TARGETS` is asked first and decides both `hasText` and the target to convert,
so the two never disagree, and an `INCR` reply is read chunk by chunk off the
`PropertyNotify`s. Sending INCR is not implemented and deliberately so: past
one request's worth (~16 MB with BIG-REQUESTS) the paste is refused rather
than truncated; `MULTIPLE` and `TIMESTAMP` are not served, and `STRING` is
UTF-8 bytes rather than Latin-1. `Tests/Graphics/X11ClipboardTests-Linux.cpp`
is 9 cases in the `X11WindowTests` binary, taking the display and the
clipboard lock both, with a second xcb connection playing another client — on
the message thread with eacp's loop pumped when it reads what we own, and on a
thread of its own when it must own the selection while `getText` blocks,
including a real 256 KB INCR sender, which keeps one transfer per requestor:
under XWayland the compositor's clipboard bridge converts a fresh selection at
the same moment eacp does, and a sender holding one requestor's state stalled
the other's read — a one-in-ten failure under load on the desktop, never on
Xvfb, and eacp's receiver was right throughout. Green on bare Xvfb and under
XWayland; the ownerless case skips on a desktop, where the compositor takes a
dropped selection back within a millisecond. Not verified by anyone yet: a paste into
or out of GTK, Qt or a DAW's own widgets, and a clipboard manager taking the
selection on exit — a copy dies with the process, as it does on Wayland.*

*Landed 2026-09-16, `Xft.dpi`, RandR change events and cursor themes. The
connection reads the root's `RESOURCE_MANAGER` at startup and selects
`PROPERTY_CHANGE` on the root, and the scale is `Xft.dpi / 96` kept as a
fraction rather than rounded to a whole factor — Qt's rule, not GTK's, so a
desktop at 150% is 1.5 and not 2. With that, `X11Window-Linux.cpp` grew the
points↔pixels seam it had not needed: the server measures a toplevel in pixels
and everything above the native in points, and the size, the position, the
`ConfigureNotify`, the translated position and `WM_NORMAL_HINTS` each convert
and round once at the crossing (`applyConstraints` had been handed pixels
while it works in points, which only ever looked right because the scale was
1). `Display` is halved into points with the scale as its `backingScale`, the
frame pacer and the view-surface children needed nothing — they were already
written against `window.scale` for `EmbeddedView` — and a `PropertyNotify` for
`RESOURCE_MANAGER` reaches every toplevel, which keeps the point size it was
laid out at, asks the server for the pixels that size now needs and fires
`backingScaleChanged` down its tree: the Wayland scale change exactly. An
`EmbeddedView`'s scale is still only what `setPixelsPerPoint` said (D6's
contract, and the Risks bullet's answer to XWayland). RandR change events are
`SCREEN_CHANGE|CRTC_CHANGE|OUTPUT_CHANGE` on the root; either event drops the
cached output, reads it again — from the root's live geometry, since xcb never
revises the setup it read at connect — and re-rates the pacer, which assigns a
new `Threads::Timer` over the old one because a `Timer`'s interval is fixed at
construction. Cursor themes turned out to need one fix and no more:
`libxcb-cursor` already honours `Xcursor.theme`, `Xcursor.size` and a size
derived from `Xft.dpi` out of the same database, and `XCURSOR_PATH` and
`XCURSOR_SIZE` out of the environment — measured with a probe that sets the
property and reads the drawn cursor back through `XFixesGetCursorImage` — but
it reads them only at `xcb_cursor_context_new`, so the context and the cursors
cached from it are now freed and built again on every database change and the
shape under the pointer re-applied. It does not read `XCURSOR_THEME` from the
environment at all — the string is not in the library — and there is no API
to pass a theme in, so that one is documented rather than fixed. Six cases: the
new toplevel's scale, a fractional 144 dpi proving the rounding holds at 1.5
for the window and for a presenting child, a database change rescaling an
open window, pointer positions in points, the RandR re-read and the cursor
size. All six rewrite the root's database or the screen and so are keyed on
`EACP_XVFB_OUTPUT` — a desktop session's database is not ours to write — and
five existing cases that compared server pixels with point numbers now scale
their expectations, which is what lets the whole suite run for real at scale 2
under XWayland on a HiDPI desktop. Not verified by anyone yet: a screen resize
actually moving the frame and the refresh, and with it the pacer re-rating onto
a new rate. Xvfb's RandR is rigid — one mode with a zero dot clock, a screen
whose maximum is the size it started at, no transforms and no new modes — so
`RRSetScreenSize` is refused there and the case drives a primary-output change
instead, which exercises the selection, the dispatch and the re-read but never
a different answer.*

*Also on 2026-09-16, a stage-3 case made honest:
`EmbeddedView/aPacedViewLeavesNoThreadBehindWhenItGoes` counted
`/proc/self/task` the instant the view was gone, and the kernel lists a
just-joined thread there a moment longer — under a loaded machine it failed
one run in three, on the stage-3 code as much as on this. It now waits up to
two seconds for the count to settle, still with no pump in between, which is
the property it exists to prove.*

*Landed 2026-09-16, XI2. `X11Connection` negotiates XInput, asking 2.2 and
requiring 2.1 — raw events are 2.0's and smooth scrolling 2.1's, and a server
with one but not the other is old enough that keeping the halves apart would
buy nothing — and `EACP_X11_NO_XI2=1` refuses it, which is how the fallback
stays a tested path rather than a hopeful one. The whole pointer moved to XI2
rather than scroll alone (GTK, Qt and SDL all do the same), and the core
pointer bits now come off the window's own mask where XI2 is live rather than
being left to the server to suppress: `getWindowEventMask()` subtracts them,
both natives take their mask from it and call `selectPointerEvents()` after
`registerWindow`, and a press delivered twice is impossible by construction.
Every `XCB_GE_GENERIC` with the extension's opcode goes straight from
`dispatch` to the seat — including raw motion, which names no window at all —
and `X11Input` does its own window lookup through the connection's map, which
is what both natives were doing by hand for the core events. The seam that
made this cheap is that the core handlers were split into neutral ones
(`pointerMovedTo`, `buttonAction`, `pointerEntering`/`Leaving`,
`dispatchMotion`) taking a point and a button number, so the XI2 path is
protocol decoding and nothing else. Scrolling is `xcb_input_xi_query_device`
read for each device's `ScrollClass`, re-read on `XI_DeviceChanged` and
`XI_Hierarchy`, with a per-axis last value so each report is a difference and
the first after a change is a baseline that scrolls nothing; the difference
over the increment is clicks, and the buttons 4–7 the server emulates beside
it carry `XIPointerEmulated` and are dropped, while a device with no scroll
class — Xvfb's virtual pointer, most VMs — still arrives as those buttons
with no flag and notches exactly as before. X11 measures scroll in clicks and
never in pixels, so a frame is always lines, fractional ones from a trackpad,
and `preciseScrolling` stays false rather than promising points nothing
measured. A mouse lock keeps its grab, its hidden cursor and its recentring
warp, and selects `XI_RawMotion` for `XIAllMasterDevices` on the root for the
length of the lock: the device's own report, delivered whoever holds the
pointer grabbed, and generated by no warp — so the recentre, which a position
could never be told apart from the hand, now contributes nothing and the
motion path stops counting deltas while raw is live. Both figures a raw event
carries are used, as Wayland's relative pointer gives both: `axisvalues` is
`delta` and `axisvalues_raw` is `rawDelta`. One thing had to change for that:
the grab's `owner_events` is now off, because with it on the server hands the
same raw event to the root's selection twice — once on the normal delivery up
from the window under the pointer and once in the delivery to every root a
raw event always gets — and every delta was doubled. Measured with a
standalone probe: no grab one event, grab with `owner_events` two with the
same sequence and timestamp, grab without one. Off is what a lock means
anyway; the cost is that a scroll while locked is wheel buttons rather than
valuators. `X11WindowTests` is 47 cases now: the lock case drives relative
XTest motion instead of a warp and asserts `rawDelta`, three new ones cover
the warp counting nothing, the core bits being absent from the window's mask
with one press arriving once, and the whole thing again under
`EACP_X11_NO_XI2`, and two need no display at all because neither server this
suite can run on has a device with a scroll class — Xvfb's virtual pointer
has none and XWayland's seat is the compositor's — so the valuator arithmetic
and the emulated-button filter are checked as functions. `with-xvfb`
X11+EmbeddedView+Present 74/74, headless 1616/1616, XWayland 64/64 at scale 2.
Not verified by anyone yet: a physical wheel or trackpad actually scrolling,
and nothing scrolling twice, which is the one thing no server here can be
made to do — what was checked on the desktop is that XWayland's device table
reads back through the same calls (valuators 2 and 3, increment 1.0) and that
a real `WidgetGallery` comes up with no core pointer bits on its window; also
unverified, a hotplug driving the table re-read, and acceleration differing
from the raw figure, which Xvfb's XTEST device never applies. `Present`
pacing, the stage's last item, is left as it is: it was conditional on the
pacer proving visibly worse on real hardware, and nobody has seen that.*

**Stage 6 — thin eacp host bridge — done.** D10, and `Apps/Plugins` builds on
Linux once `DemoPlugin`'s `ShapeLayerView` content is replaced with a
`GPUView`.

*As built.* The bridge is the D10 *as built* note. `Tests/Core/RootLoopTests-Linux.cpp`
is 10 `RootLoop/` cases over `RootLoopTestPlugin`, a fixture built with
`eacp_add_plugin` that exports C entry points which defer work inside its own
copy: hosted `callAsync`/`callAfter`/`Timer` delivering under the test
binary's loop, the first `callAsync` finding the root loop with no explicit
attach, the hosted copy seeing the root loop as running, an `Apps::run<T>`
inside the plugin constructing under the root loop and its `Apps::quit()`
returning the root `runFor`, the marker cleared when the loop exits, the
foreign-host shape (only the fixture's own `getEventLoopFd`/`pumpEventLoop`
move it) attaching nothing, `Plugins::unload` and a bare
`DynamicLibrary::close()` each detaching across 20 real unmaps, and two
hosted copies sharing one attachment. Core suite 251/251, an ASan build of
Core 256/256, headless 1857/1857, `with-xvfb` 74/74, `with-weston` 23/23.
`DemoPlugin` and `PluginHost` each draw a `PluginDemo::SpinningTriangleView`
(`Apps/Plugins/SpinningTriangle.h`, the X11Plugin triangle with the clear
colour as an argument): the host's blue and still, the plugin's orange and
turned by its own 10 Hz `Timer`, so a still of the two windows says which
copy is being pumped. `Apps/CMakeLists.txt` enters `Plugins` under
`EACP_HAS_GPU` beside `GPU`, the pair is unconditional inside it and the X11
pair is `LINUX`; `DemoPlugin` links `eacp-gpu` and takes `--no-undefined` as
`X11Plugin` does; `PluginHost` tears down through `Plugins::unload` with
`demo_close_window` as the quiesce and quits from a `callAsync` queued behind
the deferred unmap. Under Xvfb the plugin's `callAsync` and timer lines now
print and it presents 9 frames in its second; under headless Weston the host
is Wayland, the plugin copy takes X11, finds no `DISPLAY` and falls back to
the headless native, and the same lines print. Neither the pair on macOS or
Windows nor an actual screenshot has been checked; `framesRendered` is the
evidence. Seen and not chased: under Weston about a second passes between the
plugin's quiesce and the host's quit line (the deferred unmap sits between
them), where under Xvfb it is milliseconds.

**Docs, with each stage:** `CLAUDE.md` ("The Linux Backend"), README's module
table and the Linux paragraph, `GPU/README.md`'s surface section, the CI
`apt-get` line (`libxcb1-dev libxcb-xkb-dev libxkbcommon-x11-dev
libxcb-randr0-dev libxcb-xfixes0-dev libxcb-cursor-dev libxcb-icccm4-dev
libxcb-xinput-dev libxcb-xtest0-dev xvfb`), the `Dockerfile`.

## 5. Risks and open questions

- **Keyboard focus in hosts** is the known Linux plugin sore spot (D6). The
  focus-on-click rule is the industry answer; some hosts still eat keys.
  Nothing in eacp can fix a host that grabs the keyboard; document it.
- **Scale under XWayland.** A DAW under XWayland on a 2× desktop is either
  upscaled by the compositor (blurry, but the host says scale 1) or told the
  real scale by the host. `setPixelsPerPoint` covers both; standalone X11
  toplevels read `Xft.dpi` and otherwise assume 1.
- **Two connections in one copy** (D3) is allowed but only the tests will
  do it. If it turns out to cost complexity in `LinuxWindowSystem`, restrict
  to one backend per copy and make the tests two binaries.
- **Mesa's WSI shares our xcb connection** (as it shares the `wl_display`
  today): it registers special event queues for Present, and reads from the
  socket. The `prepare` step draining `xcb_poll_for_queued_event` is what
  keeps events it pulled in from waiting until the next readiness; this is
  the xcb twin of the `wl_display_prepare_read` dance and needs the same
  care.
- **Xvfb versus XWayland-under-Weston** for the CI lane: Xvfb is lighter and
  deterministic; if lavapipe presentation misbehaves on it (MIT-SHM is
  usually available, `xcb_put_image` is the fallback), Weston's
  `--xwayland` is the alternative and the scripts are the only thing that
  changes.
- **Threads for timers.** `Timer`/`DisplayLink` each own a thread. With epoll
  in place, `timerfd` would fold them into the loop with no thread per timer;
  an improvement, not a requirement, and not in these stages.
