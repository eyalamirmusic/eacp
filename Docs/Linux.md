# The Linux backend

Linux draws through Wayland or X11, Vulkan and FreeType, and it is on wherever
the graphics modules are built — `EACP_HAS_DRAW`, `EACP_HAS_GPU` and
`EACP_HAS_TEXT` are true there exactly as they are on Apple and Windows (see
[Build.md](Build.md) for the capability variables). What it does not have is
the platform's own 2D tier, `EACP_HAS_CONTEXT`. This page is the account of
what the backend is and how to run it; `Lib/eacp/GPU/README.md`'s "Linux"
section is the Vulkan half in detail, and `CLAUDE.md` is the file-by-file
map of the window-system seam.

## What it is

The backend is three things. An `eacp-graphics` with two window systems in
it: on Wayland a `Window` is a `wl_surface` with an xdg-shell
toplevel decorated by libdecor, the view tree, hit-testing and input routing are
the portable ones with the seat's pointer and keyboard translated into them
through xkbcommon, a `GPUView` gets a `wl_subsurface` of its own kept at its
bounds and scaled by the compositor's fractional scale, `Display` reports the
first output, mouse lock goes through pointer-constraints, the clipboard is a
`wl_data_device` on the seat (text and `text/uri-list`, installed into
`Core`'s `Clipboard` through a backend hook so `eacp-core` still links no
Wayland), a compositor that goes away mid-session tears every window down
through the same `onLost` path a hidden view takes and leaves the process
running headless, and the display's connection is pumped by eacp's own event
loop. What a window cannot do there is what the protocol has no words for — a
position, a raise, an icon — and the file says so where it matters. On X11 a
`Window` is an xcb toplevel — no Xlib anywhere — carrying the ICCCM and EWMH
properties a window manager reads, with an `xcb_create_window` child per
presenting view, the keymap taken from the server through `xkbcommon-x11` so
layouts and dead keys behave as they do on Wayland, mouse lock as a pointer
grab and a warp to the centre, and frames paced by a timer at the RandR mode's
rate because X11 has no frame callback — re-rated when a RandR change moves
that rate; a position is a real one there, the display's scale is `Xft.dpi`
over 96 read from the root's `RESOURCE_MANAGER` and followed when it changes,
kept as a fraction so 150% is 1.5, an
`EmbeddedView` is an `xcb_create_window` child of a window id its host owns
whose scale is whatever that host says, and
the clipboard is the `CLIPBOARD` selection owned by a 1x1 window that is never
mapped, so a copy needs neither a toplevel nor keyboard focus to take it (text
and `text/uri-list`, `TARGETS`, INCR on the receiving side, behind the same
backend hook as the Wayland one). Both backends sit behind one window-system seam
(`LinuxWindowSystem`, `LinuxWindowNative`, `LinuxWindowSurface`,
`ViewSurfaceBackend`, `LinuxInput`, `LinuxSeat`) and both are compiled into
every copy, so which one a window gets is a runtime decision:
`EACP_WINDOW_SYSTEM=wayland|x11` overrides, and otherwise a plugin copy takes
X11 while a standalone app takes Wayland when a compositor answers and X11 when
none does. That embedded surface, together with an event loop that is one `epoll`
descriptor with a pump (`getEventLoopFd`, `pumpEventLoop`) a plugin host's own
loop can drive, is what audio-plugin hosting on Linux needs —
`Apps/Plugins/X11Host` and `X11Plugin` run the whole path in-tree, a window id
and four C functions apart. When the host is itself an eacp app even those
four are not needed: the copy running the root loop advertises a bridge in the
process environment, a copy loaded from a dynamic library attaches its
descriptor to it on the first thing it defers, and its windows, timers and
`callAsync`s then run off the host's loop with no code on either side —
`Apps/Plugins/PluginHost` and `DemoPlugin`, now a `GPUView` pair, show it. A Vulkan backend under it: everything from `Device`
to `RenderPass` is real, the drawable `Frame` renders
into a swapchain image and presents it, and `GPUView` owns that swapchain —
mailbox or FIFO, frames in flight, rebuilt on resize and `OUT_OF_DATE`, with
continuous rendering paced by the compositor's frame callbacks on Wayland and
by that same timer on X11 rather than by a clock of the renderer's own — beside
the off-screen render-and-read-back path every pixel test rides. And a text
stack beside them: `eacp-text`'s glyph rasterizer on FreeType, HarfBuzz and
fontconfig, so `Sprites`, `UI` and the portable half of `SVG` build and run
too — a whole widget tree, its text, its images and its SVG documents drawn
inside one `GPUView` through the coverage rasterizer and the glyph atlas.

What Linux still does not have is the platform's own 2D tier. There is no
`Graphics::Context` and no `Graphics::Font` — `Path` exists, but only as
recorded geometry — so the retained `ShapeLayer`/`TextLayer` and the views over
them, `TextInput`, the image codecs (an `Image` is a pixel container there, and
loading a file yields an invalid one), menus and the tray are absent or honest
stubs. `SVG`'s native-layer builder and the `SVG::parse`
in front of it go with them; the same document parses and draws through
`SVGComponent`. Under `EACP_HEADLESS=1`, or with neither display server to
reach, every window is built and never shown and every GPU test still runs on
Mesa's lavapipe with no display server at all; the window and present tests run
for real under a headless Weston, and again under an Xvfb for X11, which is
where input is exercised — Weston's headless backend has no seat and Xvfb has
one.

## Dependencies

The Vulkan half needs no new build dependency: the headers, `volk` and the
allocator are fetched by CPM, and the loader is opened by name at runtime, so
all a machine needs to run it is a driver — `mesa-vulkan-drivers` is enough, and
its software rasterizer is what CI uses. `EACP_VK_SOFTWARE=1` asks for that
device by preference; `EACP_REQUIRE_GPU=1` turns "no device" from a suite that
silently skips into a suite that fails. The Wayland, X11 and text halves are
found the way libcurl is, by pkg-config against the machine's own libraries:
`libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev
libdecor-0-dev libxcb1-dev libxcb-xkb-dev libxkbcommon-x11-dev
libxcb-randr0-dev libxcb-xfixes0-dev libxcb-cursor-dev libxcb-icccm4-dev
libxcb-xinput-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev
pkg-config` on Debian/Ubuntu,
`weston` and `xvfb` to run the window tests without a desktop
(`Scripts/with-weston` and `Scripts/with-xvfb`, which are also `with-weston` and
`with-xvfb` in the image) with `libxcb-xtest0-dev` for the X11 suite's own
synthetic input, and fonts for the text tests to resolve —
`fonts-dejavu-core fonts-dejavu-extra
fonts-droid-fallback fonts-noto-color-emoji`. `EACP_REQUIRE_DISPLAY=1` does for
the display server what `EACP_REQUIRE_GPU=1` does for the device, and
`EACP_REQUIRE_FONTS=1` does it for the fonts, which is the third way a suite can
report green by skipping everything.

The HTTP client is libcurl on Linux, so a build needs its development headers
too (`libcurl4-openssl-dev` on Debian/Ubuntu).

## Building without it

`-DEACP_BUILD_GRAPHICS=OFF` builds the portable half on Linux as on any other
platform — `Core`, `Network`, `SIMD`, the shader EDSL and emitters, the CPU
compute interpreter, the ML graph builder — and is the only switch that turns
the graphics modules off; there is no Linux-specific one. How CI builds and
tests the backend, and the Docker commands that reproduce its three steps, are
in [Build.md](Build.md).

## Environment variables

| Variable | What it does |
| --- | --- |
| `EACP_WINDOW_SYSTEM=wayland\|x11` | Overrides the backend a window gets. Otherwise a plugin copy takes X11 and a standalone app takes Wayland when a compositor answers, X11 when none does |
| `EACP_HEADLESS=1` | Builds every window with no surface and never shows it; the GPU tests still run on lavapipe with no display server |
| `EACP_VK_SOFTWARE=1` | Prefers a CPU Vulkan device (Mesa's lavapipe), mirroring `EACP_D3D12_WARP` |
| `EACP_VK_VALIDATION=1` | Enables `VK_LAYER_KHRONOS_validation` with a debug-utils messenger that logs |
| `EACP_X11_NO_XI2=1` | Refuses XInput 2, so the X11 backend takes the core pointer path |
| `EACP_REQUIRE_GPU=1` | Makes `GPUTests` fail rather than self-skip when no device came up |
| `EACP_REQUIRE_DISPLAY=1` | Makes the window and present tests fail rather than self-skip without a display server |
| `EACP_REQUIRE_FONTS=1` | Makes the font tests fail rather than self-skip when fontconfig resolves nothing |
