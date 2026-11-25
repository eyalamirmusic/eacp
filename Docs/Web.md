# The web target

The web target is eacp compiled to wasm32 with Emscripten, running in a
browser page or under node. So far it is `Core` and `Network`, the
device-free pieces that build everywhere — `eacp-gpu-codegen`,
`eacp-cpu-compute`, `eacp-ml-graph` and `eacp-webview-bridge` — and
`Graphics`: `EACP_HAS_DRAW` is on, a window being a `<canvas>` (see "Windows,
views and input"), and so is `EACP_HAS_GPU`, over WebGPU (see "The GPU"),
while the other drawing capability variables are still off (see
[Build.md](Build.md)), so `Text`, `UI` and everything above them are not
built. This page is the account of what the target is, how it is
built and run, and where it behaves differently from the other platforms;
`CLAUDE.md` has the short version.

## The stance

The target is browser-first, and every decision follows from that. A program
is a guest on the browser's own event loop rather than the owner of one: there
is one thread, the page's, and eacp's loop is the browser's loop. Nothing is
compiled with `-pthread`, which is what keeps the output deployable as plain
static files. Threads on the web are workers sharing a `SharedArrayBuffer`,
which a browser hands out only to a cross-origin isolated page, one served with
the `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers;
a build without threads needs no such headers and runs from any static host.
Networking is asynchronous first, on the runtime's own `fetch()` and
`WebSocket`, because that is the only kind the platform has: the blocking
calls exist, but only where an executable has been linked to suspend (below).

`Platform::current()` is `OS::Web` there, `Platform::isWeb()` is true,
`architectureName()` is `"wasm32"`, and `isPosix()` is true as well, because
Emscripten's musl is the POSIX API the `-Posix` sources compile against — with
no processes, signals or dynamic libraries behind it. The top-level
`CMakeLists.txt` lets wasm32 through its 64-bit check (wasm64 is not what
browsers run by default) and includes `CMake/Web.cmake` before any target
exists, dependencies included, because every object in a web link has to be
compiled with the same exception model: `-fwasm-exceptions`, wasm's own.

## Building and running

A web build is a cross-compile with Emscripten's toolchain file, either named
directly or through `emcmake`:

```bash
cmake -G Ninja -B build-web -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DCMAKE_TOOLCHAIN_FILE=<emscripten>/cmake/Modules/Platform/Emscripten.cmake

emcmake cmake -G Ninja -B build-web -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
```

where `<emscripten>` is `$EMSDK/upstream/emscripten` for an emsdk install, or
`$(brew --prefix emscripten)/libexec` for Homebrew's. Node runs the tests and
the launchers, as `CMAKE_CROSSCOMPILING_EMULATOR`, which `Web.cmake` does not
look for itself: `emcmake` names it, and the toolchain file alone takes the
`node` on `PATH`. Where neither found one — an IDE started from the Dock has no
shell `PATH` — the emulator is the test node (see "Testing").

`Apps/` goes through the same capability variables there as everywhere, so
what builds is what needs no text or 2D tier and no socket of its own: the
three `Apps/Web` examples written for the browser, `HelloLoop`,
`HelloNetwork` and `HelloInput` (a window on a canvas printing the pointer,
wheel and key events its view receives; it never exits, so it has no ctest
entry, and `HelloInput-run` is how to try it), and five examples of every
platform, unchanged — `Console`,
`ZipDemo` and `OnlineResourceDemo` from `Apps/Console`, `Http` and `BasicHttp`
from `Apps/Network` — and the `Apps/GPU` examples that paint no 2D
overlay: `Triangle`, `TriangleGen`, `RotatingTriangle`, `Teapot`,
`ComputeParticles`, `ComputeImage`, `Paths`, `PathCoverage`, `Texture`,
`Maze`, `Sprites`, `Clipping`, `StreamingStress`, `TextureAtlas` and
`HotReloadGPU` in a window, and `Compute`, `AsyncCompute`, `CpuCompute` and
`PathBench`, which compute and exit. The servers, TCP and IPC examples are behind
`EACP_HAS_SOCKETS` and `Apps/Plugins` is never added. An app joins the web
by being an `eacp_add_app`, the same call that makes it everywhere else, and
on the web produces `<app>.js`, with the wasm inside it (or beside it as
`<app>.wasm`, see `EACP_WEB_SINGLE_FILE` below); its CMake has no web branch:

```cmake
eacp_add_app(HelloLoop Main.cpp
        DESCRIPTION "callAsync, callAfter and a Timer on the page's own loop.")
target_link_libraries(HelloLoop PRIVATE eacp-core)
```

Under Emscripten `eacp_add_app` calls `eacp_web_executable` itself, handing
it the three keywords only that branch reads — `NESTED_PUMP`, `CONSOLE` and
`COMPANION <script.mjs>` — while `DESCRIPTION` is a general one, kept on every
platform as the target's `EACP_APP_DESCRIPTION` property, whose readers so far
are the app's page and the gallery. A plain `add_executable`, which the test harnesses are, calls
it directly.

`eacp_web_executable(<target> [NODE_FILESYSTEM] [NESTED_PUMP] [CONSOLE]
[NO_GALLERY] [DESCRIPTION <text>] [COMPANION <script.mjs>]
[ARGUMENTS <arg>...])` gives it the link options every
web executable shares (`eacp_web_link_options`): `-sALLOW_MEMORY_GROWTH=1`, a
1 MB stack (`-sSTACK_SIZE=1MB`) and `-sEXIT_RUNTIME=1`, so the runtime exits
with the code `Apps::quit` or `main()` gives it, and `CMake/Web/UncaughtException.js`
as a `--post-js` (below, "Uncaught exceptions"). `NODE_FILESYSTEM` adds
`-sNODERAWFS=1`, node's own file system under the program, which makes the
executable node-only; the test suites link that way, an app does not. Every
other executable, harnesses included, is linked `-sSINGLE_FILE=1` while
`EACP_WEB_SINGLE_FILE` is on (the default): the wasm is embedded in the `.js`
as UTF-8 text (Emscripten's binary encoding: Teapot's debug `.js` is 13%
larger than its `.js` and `.wasm` were, where base64 would add a third), so
the page loads one script and fetches nothing, and runs from a `file://`
URL. The script must then be read as UTF-8:
the pages say `<meta charset="utf-8">`, which a classic script inherits, and
`Launch.mjs` serves `.js` with `charset=utf-8`. `OFF` keeps the `.wasm` beside
the `.js`, which a page can fetch only over http. The node-only suites and
`GPUTests` (which runs in headless Chrome but is not an `eacp_web_executable`)
keep their `.wasm` either way.
`NESTED_PUMP` links `eacp-core-nested-pump` (see "The nested pump"), which is
how an example that blocks in `main()` — `OnlineResourceDemo`, `Http`,
`BasicHttp` — runs unchanged, at the price of needing JSPI.

What else an app is follows from what it links, read once its links are all
made (`eacp_web_links_target`, which walks `LINK_LIBRARIES` and
`INTERFACE_LINK_LIBRARIES` through every target reached, aliases resolved and
generator expressions skipped). One that links `eacp-gpu` is a WebGPU app: its
headless runs give Chrome its GPU (`Launch.mjs --webgpu`, which otherwise
passes `--disable-gpu`), and its ctest entries also fail on a `WebGPU: ` line,
a validation error the device reported. One that links no `eacp-graphics` is
a console app, which prints rather than draws; `CONSOLE` says so of one that
does link it and opens no window, as the GPU examples that compute and exit
do, since `eacp-gpu` links `eacp-graphics`. `DESCRIPTION` (else the
`eacp_add_app` one) and `NO_GALLERY` (a test harness) are for the page and
the gallery (below, "The host page"). An app's build output is the `.js` (and
the `.wasm` beside it when not single-file) and its own `<app>.html`, which
runs it; a deployer may equally write a page of their own that defines
`Module` and loads the `.js`.

An executable that can run in a browser also gets one launcher target,
`<app>-run`, not part of `all`: it builds the app and opens its `<app>.html`
in the default browser. It is `CMake/Web/Launch.mjs`, plain node with no
packages. A single-file app with no `COMPANION` is opened as a `file://` URL
(`open` mode) and the target returns at once: no server, and the output and
exit code stay in the page. One with a companion, or any app with
`EACP_WEB_SINGLE_FILE` off, goes through `browser` mode, which serves the
app's directory on a loopback port, opens the page from there and relays the
page back to the terminal (below). The same script has a `node`
mode, which runs the app under node, and a `headless` mode, which does what
`browser` does in the headless Chrome named by `--chrome` (the ctest entries
pass `EACP_WEB_CHROME`), failing with 124 if the app has not exited within
120 s, and at once with no Chrome. No target runs those two; the app's ctest
entries do (see "Testing"), and by hand, from the app's build directory, they
are

```bash
node CMake/Web/Launch.mjs node <app>.js [--companion <script.mjs>] [-- <args>...]
node CMake/Web/Launch.mjs headless <app>.js --chrome <chrome> [-- <args>...]
node CMake/Web/Launch.mjs open <file.html>
```

with `CMake/Web/Launch.mjs` given by its path in the source tree. `open`
hands the file's `file://` URL to the platform's opener (`open`, `cmd /c
start`, `xdg-open`) and exits, 1 when the opener could not start; which
program shows it is the one the system has for `.html`, normally the default
browser. In the
browser modes it serves the app's directory as static files and a page that
hands the program its arguments through `Module.arguments`, relays
every line it prints and its exit code back to the launcher, and exits with
that code — so the browser run prints what the node run would. The launcher
keeps relaying for 1 s after the page reports its exit, and a later non-zero
code (an abort's 1) overrides an earlier 0, so something that runs after the
exit still fails the run; every browser run takes about a second longer for
it. An app that
never exits is served until the launcher is interrupted. `COMPANION` names a
node script started first as `node <script> serve`, whose printed `baseUrl` is
appended to the app's arguments; that is how `HelloNetwork` gets the test
server it talks to. `eacp_web_executable` keeps `COMPANION` and `ARGUMENTS` as
the target's `EACP_WEB_COMPANION` and `EACP_WEB_LAUNCH_ARGUMENTS` properties,
which are read when a launch command is made: for `<app>-run`, by a call
`eacp_web_executable` defers to the end of the app's directory, so the app's
links are known, and by `eacp_web_add_app_tests` for the ctest entries (see
"Testing"), which `Tests/` makes after `Apps/`. They are therefore given to
`eacp_web_executable`; a property set on the target afterwards reaches only an
`eacp_web_add_app_tests` called after that.

## The host page

Every executable that can run in a browser gets a page of its own,
`<app>.html` beside its `<app>.js`, harnesses included
(their `<app>-run` opens it). It is `CMake/Web/Host/app.html` with the app's
settings written in at the end of the app's directory, when its links are
made and so its `webgpu` and `console` flags can be read off them: a deferred
call fills the `@EACP_WEB_PAGE_NAME@`, `@EACP_WEB_PAGE_DESCRIPTION@` (both
HTML-escaped) and `@EACP_WEB_PAGE_CONFIG@` placeholders with
`string(CONFIGURE ... @ONLY)`, which leaves JS's `${...}` alone, and writes
the result with `file(GENERATE)` to `$<TARGET_FILE_DIR>`, so a multi-config
generator gets one per configuration. The config is the JSON object `{name,
page, script, description, webgpu, jspi, console, singleFile, companion,
arguments}`,
its strings escaping `<`, `>`, `&` and `$` as `\u` escapes so nothing filled
in can close the `<script>` or read as a generator expression; `script` and
`page` are generator expressions added after the escaping. The template is a
`CMAKE_CONFIGURE_DEPENDS`, so editing it reconfigures. One self-contained file,
CSS inline, no bundler and no package; light and dark follow
`prefers-color-scheme`.

The page shows the app's name, description and badges (WebGPU, JSPI,
console, a companion), checks what the app needs before loading it — WebGPU
(`navigator.gpu`) for a `webgpu` app, JSPI (`WebAssembly.Suspending`) for a
`jspi` one — and says so instead of loading when the browser lacks it, as it
does when opened from `file://` with its `.wasm` in a file of its own
(`singleFile` false), which it could not fetch. It then
sets `Module` (`arguments`; `canvas`, a canvas filling the page under the
header; `print` and `printErr` into an output pane; `onExit` and `onAbort`
into a status line) and loads `<app>.js`. The app's first window takes that
canvas, as any page canvas (see "Windows, views and input"): the canvas is
hidden until it is claimed, so a console app shows its output pane alone, and
a window's app gets the canvas with the pane as a strip beneath it. The
page's CSS sizes the canvas with `!important`, so the page, not
`Window::setSize`, is the window's size, read back through the
`ResizeObserver`. A second window appends its own canvas below. The window
title is the page's `document.title`, which the runtime sets. Query
parameters: `arg`, one argument each, replaces the default arguments when any
is given; `gallery=1` shows "← Examples", a link to `../index.html`, which
the gallery's list adds (a parameter rather than a probe for the list, so a
page opened alone, from `file://` included, makes no request to find out);
`force=1` runs an app with a companion server (`HelloNetwork`), which a
static host cannot have, and which the page otherwise refuses with a "Run
anyway" button; and `relay=1`, which `Launch.mjs` adds, posts every line and
the exit code to `POST /events`, exactly as the launcher's own page does.

Nothing else in the page or the runtime needs http. Opened from `file://`
the page's origin is `null`, which changes two things only: Chrome counts
`file://` as a secure context, so `navigator.gpu` and JSPI are there as over
http, and a `fetch()` to another site is a cross-origin request from `Origin:
null`, which the server's CORS must allow (`httpbingo.org`, which `Http` and
`BasicHttp` use, echoes the origin, `null` included).

Each app is a page of its own because nothing else would hold: the `.js` is
not built `-sMODULARIZE`, so the runtime and every JS library keep their
state on the one global `Module`, and `-sEXIT_RUNTIME=1` with `quit()` ends
that runtime for good. A page is a fresh global and a restart is a reload.

`eacp_web_executable` records every app it is given in the global property
`EACP_WEB_GALLERY_APPS` (unless `NO_GALLERY`), and a call deferred to the end
of the top-level configure (`cmake_language(DEFER)` in `Web.cmake`) writes
the manifest and the list page and adds two targets, neither part of `all`:

- `web-gallery` builds every listed app and lays out `<build>/web/` as a site
  that opens from `file://` or any static server as it is: `index.html`,
  `apps.json`, and `<app>/<app>.html` with its `.js` (and `.wasm`, when not
  single-file) for each app. Each entry of `apps.json` is the page's config
  object, `page` and `script` relative to the gallery and `companion` the
  script's file name, empty for none. `index.html` is `Host/index.html` with
  the same manifest written into its `@EACP_WEB_GALLERY_MANIFEST@` the way an
  app page gets its config, so it fetches nothing; it lists the apps as cards
  linking to `<page>?gallery=1`, and under `file://` with any app not
  single-file it says to serve the folder instead. `apps.json` is kept for
  tooling. The target first removes whatever in the folder it no longer
  writes — an app no longer listed, the `.wasm`s after `EACP_WEB_SINGLE_FILE`
  is turned on (`CMake/Web/PruneGallery.cmake`, against a file list written at
  configure) — so the folder is always the current layout.
- `web-gallery-run` builds that and opens `<build>/web/index.html` in the
  default browser (`Launch.mjs open`) and returns: no server. With
  `EACP_WEB_SINGLE_FILE` off the apps then cannot load, and the folder is for
  a static server.

`<app>-run` in `browser` mode serves the app's directory alone and sends `/`
to `<app>.html?relay=1`, with an `arg` for each argument it was given (the
companion's URL included) and `force=1` when it started a companion; so it
needs no `web-gallery`, and the terminal still sees the output and exits
with the app's code. `headless` mode, the ctest entries' path, keeps the bare
page (`pageFor` in `Launch.mjs`), which sets no `Module.canvas`:
`WebWindowHarness` checks the windows' own canvases, and its runs are
unchanged.

## Run, quit, and the loop

`Threads/EventLoop-Web.cpp` is the loop. `callAsync` queues the work and
schedules one drain on a later turn of the browser's loop
(`emscripten_set_immediate`); `callAfter` keeps every pending callback in one
ordered map and arms a single JS timeout for the nearest deadline, which hands
what is due to `callAsync` — one timeout rather than one per callback, which
would let node fire two close deadlines out of order — so there is no
scheduler thread (`Threads/CallAfter.cpp` is not built there); a `Timer` is the
page's `setInterval` with each tick queued the same way, one at a time: a tick
already queued absorbs the intervals that pass before it runs, and one queued
when the timer is destroyed does nothing (`Threads/Timer-Web.cpp`).
`isMainThread()` is always true, and naming a thread, setting its priority and
asking for realtime do nothing (`Threads/ThreadUtils-Web.cpp`).

The interval and the timeout are the plain JS `setInterval` and `setTimeout`,
not `emscripten_set_interval` and `emscripten_set_timeout`, because those hold
a runtime keepalive. `quit()`'s `emscripten_force_exit` zeroes the keepalive
count, so a timer cleared during static destruction afterwards — a static
`Timer`, or the network's transfer checker while a fetch is in flight — would
assert and abort the exit; and a pending `callAfter` would hold the runtime,
and with it a node process, alive after `main()` returned with no loop. Neither
needs a keepalive of its own: a tick or a due callback only reaches
`callAsync`, whose queue runs only under a loop, and every loop that outlives
`main()` holds the runtime alive itself.

Unless the executable links `eacp-core-nested-pump` (below),
`EventLoop::run()` does not return: it unwinds the stack back to the browser as
an exception would, with the runtime kept alive
(`emscripten_exit_with_live_runtime`). The destructors of the locals of every
frame from `main()` down to `run()` run before the app is constructed on a
later turn, a `catch (...)` around the call does not stop the unwinding, and
nothing after `run()` executes. `Apps::run<T>()` still constructs the app on
the loop's first turn; what it does after its loop elsewhere — destroy the
app, stop the shared `ThreadPool` and the `callAfter` timers, return the
value — is done by `quit()` instead: it schedules a turn that does the same and
exits the runtime with the return value (`emscripten_force_exit`). `run(func)`
and `runConsole` work the same way, since both quit once their callback
returns. In an executable that links the nested pump, `run()` returns after
`quit()` as it does elsewhere.

So `main()` must hold nothing in a local that the app needs, and a callback
must not refer to `main()`'s locals or to the arguments of a function that
called `run()` — `run(func)` and `runConsole` copy their callbacks for exactly
this reason (`App/App.cpp`). State lives in the app struct, in what a callback
owns by value, or in statics, which stay alive with the runtime. With no loop
at all — before one starts, or after `quit()` — deferred work stays queued, as
it would in a queue nothing pumps, until a loop starts.
`attachCurrentThreadAsMain()` does nothing, since a page has no host loop to
attach to, and `detachCurrentThreadAsMain()` stops the `callAfter` timers and
the shared pool, as on the platforms with no hosted loop
(`EventLoop-Default.cpp`). `stopEventLoop()` ends the innermost `runFor` and
never the root loop, as on Windows, and `stopProcessRootLoop()` does nothing: a
page has one eacp copy.

## The nested pump

A nested pump — `runEventLoopFor`, `runEventLoopUntil`, and what waits on
them, `Async::waitFor` and the blocking network calls — waits for the loop
from inside a call, which the browser's main thread cannot do. It can only
suspend: JSPI (`-sJSPI`, an engine's `WebAssembly.Suspending`) parks the wasm
stack on a promise while the browser carries on, and resumes it when the
promise resolves. Only an executable linked with `-sJSPI` can do that, so the
capability is an object library, `eacp-core-nested-pump`
(`Threads/NestedPump-Web.cpp`), which carries the flag in its link options and
replaces three weak "cannot suspend" functions in `eacp-core`
(`Threads/EventLoop-Web.h`). One `eacp-core` serves both kinds of executable.

Linked, `run()` and `runFor()` are real loops, as elsewhere: each drains the
queue and suspends until woken or out of time, and `run()` returns after
`quit()`. While a pump is suspended, the browser's callbacks — a `fetch()`
chunk, a timer — still enter the program, but they only queue work and wake
the pump; the queue drains in the pump, never under it. A `quit()` ends the
pumps already on the stack, and a pump started after it still waits for its
own work, so a blocking call — `httpRequest`, `OnlineResource::fetch`,
`Async::waitFor`, `runEventLoopUntil` — made after `quit()` inside a nested
pump completes, as on Linux and macOS (Windows posts `WM_QUIT` again, so there
it does not). Not linked,
`runFor(0)` drains what is queued and returns, and any positive timeout prints
an error naming `eacp-core-nested-pump` and throws `std::logic_error`.

The test suites link it, because their cases are blocking code that runs
unchanged on every other platform. An app does not by default: an app on the
web is written against callbacks anyway, and without JSPI it runs on any engine
with wasm exceptions, rather than only on one that has JSPI. An example
written blocking for the other platforms opts in with
`eacp_add_app(<app> ... NESTED_PUMP)` (`eacp_web_executable(<app>
NESTED_PUMP)` for a plain executable); its launcher and ctest entries then
run under the test node (`EACP_WEB_TEST_NODE`) rather than the emulator, since
`Launch.mjs node` runs the app under its own node.

## Uncaught exceptions

A C++ exception nothing catches ends a native program in `std::terminate`.
Emscripten hands one that leaves `main()` or a callback to its `quit_`, which
under node exits non-zero but in a page only throws it on; and a `main()`
linked with the nested pump runs as a promise, so in a page the throw became
an unhandled rejection, no exit was reported and the runtime stayed alive.
`CMake/Web/UncaughtException.js`, a `--post-js` every web executable links
through `eacp_web_link_options`, wraps `quit_` so that a C++ exception reaching
it calls `abort()` with the exception's type and message instead: it prints
`Aborted(uncaught exception: std::runtime_error: <what>)`, calls
`Module.onAbort` (which a page, `Launch.mjs`'s included, takes as exit code 1)
and ends the run at once, under node and in a page alike. The type and message
come from Emscripten's `getExceptionMessage`, which a Debug build exports; a
Release build prints `uncaught exception` alone. `ThrowFromMainHarness`
(`Tests/Core/ThrowFromMainHarness-Web.cpp`) is the regression check.

## Threads

`EACP_HAS_THREADS` is off on Emscripten and `Platform::hasThreads()` is its
compile-time twin. Code that would start a thread asks it rather than
`isWeb()`, so an opt-in threaded web build would change one answer:

- `Thread::start` prints once that the platform has no threads and runs
  nothing; the thread is never joinable and never realtime.
- A `ThreadPool` has zero threads whatever its options ask for
  (`defaultThreadCount()` is 0). Its queue runs on the main thread instead,
  one task per loop turn so the loop keeps turning between them, with
  `onMainThread` and `onCancelled` delivered as elsewhere. `waitUntilIdle` and
  `TaskGroup::wait` run the queue — or only the group's tasks — inline on the
  calling thread, delivering the main-thread callbacks as they go; one that
  finds outstanding work with nothing left in the queue to run (a group
  waiting on its own running task) returns false at once, since nothing else
  could finish it. Code running inside a pool task counts as a thread of that
  pool, as on the others: `isPoolThread()` is true there, `waitUntilIdle` and
  `stop` from inside a task assert, and `waitUntilIdleFor` returns false. A
  `TaskGroup` destroyed while its task runs, or from its own callback, is safe
  and drops that task's `onMainThread`.
- `Processes::runAsync`, `BuildRunner`, `Rpc`'s worker-thread commands and
  `GPU::ShaderWarmUp` run their job on a later turn of the loop through
  `callAsync`. For the first two that job is a launch, which fails on the web
  (below); a warm-up compiles its whole set in that one turn, so nothing is
  ever in flight for a prepare to wait on.
- The network's async calls and `OnlineResource` are not on a worker at all:
  they sit on the browser's asynchronous clients.

`EACP_HAS_SOCKETS`, also off on Emscripten, is the other half: a wasm program
opens no socket of its own and cannot listen. It leaves out
`HTTPServer/`, `TCP/`, `IPC/`, `WebSocket::Server`, the `eacp-network-json`
target and the HTTP and IPC RPC transports, and is a PUBLIC compile definition
on `eacp-network` that the `Network.h` umbrella reads to leave those headers
out. The execution glue every RPC transport shares (`Rpc/AsyncCommand.cpp`)
still builds, since the WebView page bridge sits on it.

## Networking

The HTTP client is `fetch()` (`HTTP/Http-Web.cpp` with `Http-Web.js`, a JS
library `eacp-network` adds to every link): one fetch per transfer, its body
read chunk by chunk into the response or straight into a file, and its end
handed to the loop through `callAsync`. A `Timer` alive only while transfers
are checks every 50 ms for `DownloadProgress::cancel` and the three time
limits. `asyncRequest` and `asyncDownload` (`HTTP/HttpAsync-Web.cpp`) start a
transfer and return; there is no worker, only the registry that lets
`cancelAllAsyncRequests` keep a reply from being delivered. These are the way
in on the web.

The blocking calls — `httpRequest`, `Request::perform`, `downloadTo`,
`downloadFile` — wait for the same transfer in a nested pump, so they work in
an executable linked with `eacp-core-nested-pump` and otherwise return at once
with a `Response` whose error says to link it or use the async calls.
`OnlineResource::fetch`, the blocking tier, throws instead, with the same
message. `OnlineResource::start` and `fetchAsync` work everywhere: the fetch is
split around its download behind one seam (`OnlineResourceInternal.h`), which
`OnlineResource-Threaded.cpp` runs on a worker thread on every other platform
and `OnlineResource-Web.cpp` runs as the preparation at once, the browser's
asynchronous download, and the unpacking and installing on the main thread
when it lands — a large `.zip` is unpacked in one turn of the page's loop.

What `fetch()` changes, most of it written into the public headers too:

- **CORS.** In a browser a server on another origin answers only if it sends
  CORS headers, and only the response headers it exposes
  (`Access-Control-Expose-Headers`) are seen. Node applies neither rule.
- **Headers the runtime owns.** `Host`, `Content-Length` and `Connection` are
  the runtime's own, and a request's are dropped. A `User-Agent` is sent only
  when the app sets one (`setDefaultUserAgent`, or the header); otherwise the
  request carries the browser's or node's own. A browser may drop one a
  request sets (Chrome) or send it (Firefox), which on another origin needs a
  CORS preflight the server must allow.
- **Redirects.** Followed, the limit is the runtime's (20), which `fetch()`
  cannot lower, so any positive `maxRedirects` means 20; `maxRedirects = 0`
  still fails on the first redirect. With `followRedirects` off a browser
  returns an opaque redirect with no status and no headers, so the request
  fails with an error saying so; node returns the 3xx itself.
- **Connect timeout.** `fetch()` reports no connect event, so
  `connectTimeout` bounds the wait for the response headers, as on Apple;
  `idleTimeout` counts from the last body chunk.
- **Encoding.** The body arrives decoded, so under a `Content-Encoding` the
  declared length says nothing about it: progress has no total, and
  `maxResponseSize` is checked as the bytes arrive rather than up front.
- **Cache.** `IgnoreLocalCache` is `cache: "no-store"`; `UseLocalCache` is the
  browser's own HTTP cache.
- **Uploads.** A `bodyFilePath` is read whole into memory before the request
  goes, rather than streamed.
- **Parallel chunks.** `parallelChunks` is ignored: a download is one stream,
  since the chunks are threads (`HTTP/HttpParallel.cpp`, built only where
  there are threads).

`WebSocket::Connection` is the runtime's own `WebSocket`
(`WebSocket/WebSocket-Web.cpp` with `WebSocket-Web.js`), which does the
handshake, the framing, the pings and the closing handshake itself; eacp's one
state machine (`WebSocket.cpp`) sits above it as on every platform, and
`isSupported()` is true. What a script's `WebSocket` cannot do is decided
here: it takes no headers, so a connection given any reports an error and
closes with 1006 instead of opening; a browser lets a script close only with
1000 or 3000–4999, so any other code goes as 1000; `maxMessageSize` is
checked once a message has arrived whole, since the runtime reassembles it
first; and text is sent as a string, so text that is not valid UTF-8 goes out
with each invalid sequence replaced by U+FFFD. `connectTimeout` is a JS timer
beside the socket. The closing handshake's bound, `closeTimeout`, is a
`callAfter` on every platform, holding the connection's endpoint weakly, with
no thread of its own.

Both JS libraries register an exit callback with the runtime (`addOnExit`):
`Http-Web.js` aborts every live fetch and `WebSocket-Web.js` releases every
socket. An `Apps::quit()` with an `asyncRequest` or a `WebSocket::Connection`
still in flight therefore exits cleanly with the app's code, under node and in
a page, and nothing calls into the exited runtime afterwards.

## Windows, views and input

`EACP_HAS_DRAW` is on, and `eacp-graphics` builds on the Linux-neutral seam
Android already reuses (see [Linux.md](Linux.md)): `View/View-Linux.cpp` keeps
the per-view records and syncs them, `Window/LinuxWindowNative-Linux.cpp` is
the option-derived toplevel state (`LinuxWindowState`: constraints, `resizeTo`,
activation, host visibility), `Window/HeadlessWindow-Linux.cpp` is the
surfaceless window, and the pointer state machines — the click count and its
slop, a frame's scroll, the cursor shape — are `Window/PointerTrackers.{h,cpp}`,
split out of `LinuxInput-Linux` for both to share (`KeyRepeat` and the xkb
keymap stay Linux's: the browser repeats keys itself and names their text).
`Path` is the recorded geometry Android and Linux have, and the PNG/JPEG codec
is desktop Linux's over stb (`Image/Image-Linux.cpp`). Menus, the tray, global
hot keys and layers are the Linux stubs, `GameInput` has no feed of its own
(`GameInput-Default.cpp`), and `EmbeddedView` and `NativeChildSurface` are not
built, as on Android: a page has no foreign native view to embed in.

What is the web's own is in `-Web` files, with the DOM half in one JS library,
`Window/Window-Web.js`, that `eacp-graphics` adds to every link:

- **A window is a `<canvas>`** (`Window/Window-Web.cpp`). The first window
  takes the page's own canvas when the page set `Module.canvas`, as
  Emscripten's shells and each app's `<app>.html` do; every other window, and the first when there is
  none, makes a canvas of its own, appended to the body, hidden until shown,
  and removed when the window is destroyed. A canvas gets an id when it has
  none (`eacp-canvas-<n>`), a `tabindex` so it takes keyboard focus, and
  `touch-action: none`. The window's size is the canvas's CSS size in points;
  its backing store (`width`/`height`) is that times `devicePixelRatio`, set by
  the window on every resize and scale change. A canvas's layout changing
  under it — the page's CSS on a page canvas, full screen — is read back
  through a `ResizeObserver` and applied as a resize with no constraint, the
  page's layout being the authority; a page canvas that already has a layout
  when the window takes it keeps that size. `setSize` writes the CSS size.
  `setVisible` is `display`, `setTitle` the document's title and the canvas's
  `aria-label`, `setFullScreen` `requestFullscreen()` (granted only inside a
  user gesture; `isFullScreen` follows `fullscreenchange`), `setMouseLocked`
  the Pointer Lock API, and minimise, maximise and `setPosition` keep the
  value and do nothing else. `Window::getHandle()` is the canvas's selector as
  a `const char*`. No close request ever comes from a page.
- **Headless.** With no `document` — under node — or with
  `getAppEnvironment().headless`, a window is the Linux headless native: laid
  out, sized and positioned, never shown, with no surface.
- **Input.** The canvas's pointer events (`pointerenter`/`leave`/`move`/`down`/
  `up`/`cancel`, captured on press so a drag continues outside it), `wheel`
  (non-passive, its default prevented), `keydown`/`keyup`, `focus`/`blur` and
  `contextmenu` (prevented, so the right button reaches the view) are forwarded
  to `eacp_web_window_*` exports and dispatched at once, from inside the
  browser's event, which is how a key a view kept prevents the page's default
  action (Tab, Space, the arrows) and one every view passed on does not. A
  pointer event carries every button held, so each change of that set is a
  press or a release of its own. Positions are CSS pixels from the canvas's
  top-left, which are points. Presses go through the shared click counter
  (400 ms, 5 points), a move with a button held is a drag carrying its delta,
  and the CSS cursor follows the shape of the view under the pointer. A wheel
  event in pixels (`deltaMode` 0, what Chrome reports even for a notched
  wheel) is a precise delta in points; lines and pages are notches, a page
  counting ten; the sign is flipped into `MouseEvent::delta`'s. Touch arrives
  as the same pointer events, so a finger is the left button with `fromTouch`
  set; there are no `TouchEvent`s yet. Focus and blur are the window's
  activation, and a blur forgets the keys held.
- **Keys** (`Graphics/Keyboard-Web.{h,cpp}`). `KeyboardEvent.code`, the
  physical key named for its place on a US layout, maps to `KeyCode` through
  one table both ways; `KeyboardEvent.key` is the text when it is one
  character, and Enter, Tab, Backspace, Escape and Delete type the control
  characters the other platforms give them. `isRepeat` is the browser's own
  repeat. `charactersIgnoringModifiers` and `keyCodeToCharacter` are what the
  key last typed with nothing held — the only way a page learns its layout —
  falling back to the US letter or digit. The polled `Keyboard` state is what
  the key events left behind, and `Window::isKeyPressed`'s native unit is the
  `KeyCode` itself, as on Android. Meta is `command`.
- **Display** (`Window/Display-Web.cpp`). The frame is the screen's size, the
  work area the viewport (`innerWidth` × `innerHeight`), which is what a canvas
  can cover, and the backing scale `devicePixelRatio`; all read live. A change
  of `devicePixelRatio` — a zoom, a move to another display — is caught by a
  `matchMedia('(resolution: <n>dppx)')` query re-armed each time, and every
  window takes the new scale: its backing size, then its presenting view's
  surface, then `backingScaleChanged` down the tree. Under node the display is
  the 1280 × 800 fallback at scale 1. `isSystemDarkMode` is
  `prefers-color-scheme`.
- **Frames** (`Helpers/AnimationFrame-Web.{h,cpp}`). One
  `requestAnimationFrame` per request, or a 60 Hz timeout under node, which has
  none; plain JS like the loop's timers, so it holds no runtime keepalive, and
  every pending one is cancelled when the runtime exits. `DisplayLink`
  (`Helpers/DisplayLink-Web.cpp`) re-requests one each frame and queues its
  tick through `callAsync`, coalescing as the Linux pacing thread does, so it
  stops while the page is hidden.

The listeners, the observers, the scale query and the frames are all released
when the runtime exits (`addOnExit`), so a quit with a window up calls nothing
into the exited runtime.

### The surface a presenter gets

A presenting view (`requestViewSurface`, `View/View-Linux.h`) gets the whole
canvas, whatever its bounds, one presenting view per window at a time, as on
Android (`View/WebViewSurface-Web.cpp`). The record's `NativeSurfaceHandle` is:

- `kind` — `NativeSurfaceHandle::Kind::Web`;
- `surface` — a `const char*`, the canvas's CSS selector (`"#eacp-canvas-1"`,
  or `"#" + CSS.escape(id)` for a page canvas with an id of its own),
  NUL-terminated, owned by the window and valid until the record's `onLost`.
  It is what `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector` takes;
- `window` — the window's id, never zero;
- `connection` — null.

`pixelWidth`/`pixelHeight` are the canvas's `width`/`height` attributes,
which the window keeps at its CSS size times `devicePixelRatio`, so a presenter
configures its context at that size and reads `onResized` for a change (a
resize, a scale change); it never sets the canvas's size itself. `scale` is
`devicePixelRatio`. `requestFrameCallback` is one `requestAnimationFrame`, and
`onFrameDone` runs inside it rather than on a later turn, which is where a
canvas is best drawn. `onAvailable` comes a turn after the request, once the
window is shown; `onLost` when the window is hidden or destroyed, or the view
leaves it or hides. Under node there is no canvas, so `onAvailable` never
fires.

## The GPU

`EACP_HAS_GPU` is on: `eacp-gpu` is a WebGPU backend over Emscripten's
`emdawnwebgpu` port, whose `--use-port=emdawnwebgpu` reaches every link that
takes `eacp-gpu`, and its shaders are WGSL, which `eacp-gpu-codegen` emits
itself (`ShaderBuilder` picks `ShaderBackend::WebGPU` on the web).
`Lib/eacp/GPU/README.md`, "The web", is the account; what a web app sees:

- **A device before `main()`.** A browser hands out the adapter and the device
  asynchronously, and `GPU::Device` is not, so `CMake/Web/WebGPU.js`, a
  `--pre-js` on every executable linking `eacp-gpu`, holds `main()` back with a
  run dependency until the device is there (or has failed to come), with every
  feature and the highest limits the adapter offers. Under node, or in a page
  with no `navigator.gpu`, there is none, `Device::isValid()` is false, and
  every GPU test and example stands down as on a machine with no GPU.
- **Reading back needs the nested pump.** `Buffer::read`, `Texture::read`,
  `CommandBuffer::wait`/`commit`, a shader's compile messages and a
  pipeline's validation wait for a callback, which an executable linked with
  `eacp-core-nested-pump` does by suspending through JSPI. Without it the reads
  are skipped (with a message, once), the waits return at once and a pipeline
  counts as valid; `commitAsync` and everything drawn still work. So a GPU
  example that reads results back in `main()` is a `NESTED_PUMP` one
  (`Compute`, `AsyncCompute`, `CpuCompute`, `PathBench`), and one that only
  draws is not.
- **A `GPUView` is the canvas.** It configures a WebGPU context on the
  canvas the window hands over ("The surface a presenter gets") in
  `bgra8unorm`, renders each frame into the context's current texture, and the
  page shows it when the turn returns; continuous rendering is paced by
  `requestAnimationFrame`.
- **Errors are printed.** A validation error the device reports, and a lost
  device, are printed on stderr as `WebGPU: ...`, which the ctest entries of
  an app linking `eacp-gpu` fail on.
- **Hand-written shaders are WGSL.** `ShaderSource::native(ShaderDialects)`
  takes a shader's MSL, HLSL, GLSL and WGSL and keeps the one the platform's
  backend compiles, so an example such as `Triangle` carries all four and no
  web branch.

Headless Chrome on macOS exposes WebGPU (Metal) only with its GPU process:
`Launch.mjs headless --webgpu` drops the `--disable-gpu` every other run gets
and adds `--enable-unsafe-webgpu`, which Linux needs and macOS ignores. On
Linux it also adds `--enable-features=Vulkan`, since Chrome's WebGPU runs on
Vulkan there and leaves it off; without it `requestAdapter` never answered on
the Linux machine it was tried on. A Linux machine with no GPU, a CI runner
among them, then gets Chrome's own SwiftShader adapter (`google swiftshader`,
a fallback adapter) with no flag naming it.

## Files

An app's file system is Emscripten's in-memory one: in a page it starts empty
and nothing written to it survives a reload, and an app run under node through
`Launch.mjs node` gets the same in-memory file system, not the host's. Nothing
persists yet. `FilePath`'s roots come from `FilePath-Linux.cpp`, reused, over
Emscripten's default environment (`HOME` is `/home/web_user`), so
`appSupportDirectory()`, `appCacheDirectory()` and `tempDirectory()` resolve
to paths inside that file system; `tempDirectory()` drops a trailing separator
from `TMPDIR`, which a build under node inherits from macOS. The test suites,
linked with `-sNODERAWFS=1`, see node's real file system and the host's
environment instead.

`Files::executablePath()` is the program's `.js` (`Utils/Files-Web.cpp`):
node's absolute path for it, or in a page the path of its URL, the nearest a
page comes to an executable's path. `resourcesDirectory()` is its directory.
`MemoryMappedFile` is the POSIX one, and Emscripten maps by copying into
memory, so a view larger than wasm32's address space cannot exist.

## What is stubbed

Each of these answers the way the other platforms without the feature do, so
callers need no special case:

- **Processes**: `Process-Default.cpp`, the source iOS shares, launches
  nothing; `launched()` is false and `run()` returns a result whose `launched`
  is false.
- **Dynamic libraries and hot reload**: `DynamicLibrary` is always invalid,
  with "dynamic libraries cannot be loaded on the web" as its reason.
  `eacp_add_plugin` stops the configure with a `FATAL_ERROR`, since the web
  loads no dynamic libraries, so a web build has no plugins.
  `eacp_add_hot_reload` is always static there, and the hot-reload sources
  stay in the web `eacp-core`.
- **Role**: one wasm module with no dynamic linking, so the copy is always the
  application and the only image (`ModuleInfo-Web.cpp`); `EACP_CLAIM_COPY`
  finds nothing to claim.
- **Clipboard**: the Linux clipboard with no backend installed: text reads
  empty and copies fail.
- **Alerts**: the headless path; an alert is answered with its cancel button,
  else its default one.
- **File pickers** return `nullopt`, and `openWithSystem` and
  `revealInFileManager` return false.
- **`openExternalURL`** opens a new tab in a page and does nothing under node.
- **Launch at login, the Dock, badges, power-off notice, signing** do nothing
  or answer false.

## Testing

`Tests/` builds `CoreTests`, `NetworkTests`, `GPUCodegenTests`,
`CpuComputeTests`, `MLGraphTests`, `ScriptHostTests`, `GraphicsTests` and
`GPUWidgetsTests` (whose device cases stand down there) for
the web, each
linked with `eacp-core-nested-pump` and `-sNODERAWFS=1`, and runs them under
node. The node-only link options are given per target, to the suites and
`CpuComputeBench`, not to the whole `Tests/` directory, so an executable there
that must also run in a page is not made node-only. What a platform without threads, processes or modules cannot run is left
out of the source lists or self-skips: `ThreadTests` and `ThreadPoolTests` give
way to `ThreadlessTests-Web.cpp`, which checks the refusing `Thread`, the
zero-thread pool, its inline waits, a running task counting as a pool thread,
groups destroyed from their own task or callback, and `runAsync` without a
thread; `ConsoleTests`, `BuildRunnerTests`, `DynamicLibraryTests`,
`ReloadableLibraryTests`, `HotReloadLoopTests`, `ReloadableTests` and the
modules they load, the module-role and module-info harnesses and the whole
`ProcessTests` binary are not built; the cases that drive a second thread, and
the FIFO and 2 GiB mapping cases, return early. In `NetworkTests` every case
that starts a server of its own is behind `EACP_HAS_SOCKETS`, so the web build
has the pure
cases and three web suites instead (`HttpWebTests-Web.cpp`,
`OnlineResourceWebTests-Web.cpp`, `WebSocketWebTests-Web.cpp`), which talk to
`Tests/Network/Web/TestServer.mjs`, a node server on two loopback ports (so a
redirect can cross origins) that sends CORS headers unless asked not to. ctest
starts it as a fixture (`WebTestServerStart`/`WebTestServerStop`) for the
groups that need it, and the cases read its addresses from the state file it
writes. `GraphicsTests` runs headless, as its `TestMain` asks on every
platform, so under node it is the suite desktop Linux's first CI lane runs:
the portable cases, the image codec over stb, the shared pointer trackers
(`PointerTrackerTests.cpp`, Linux's too) and the web's key table, key text and
polled key state (`KeyCodeTests-Web.cpp`); its three cases that start a
thread return early. The canvas itself is `WebWindowHarness`'s, below.

NanoTest registers a cross-compiled suite as one entry, since it cannot run it
at build time. `eacp_web_discover_tests` (`CMake/Web.cmake`,
`CMake/Web/DiscoverTests.cmake`) does instead: when ctest reads the directory
it runs `<suite>.js --list-tests` through node and registers one entry per
case group, `--test-group <group>`, as `NANO_TEST_GROUP_BY PREFIX` does
natively, and `CTestCustom.cmake` tells ctest to ignore the whole-suite
entries. A suite that cannot be listed — not built, crashed, listed nothing —
is one failing entry, `<suite>.discovery`, carrying the reason, rather than a
quiet zero.

```bash
cmake --build build-web
ctest --test-dir build-web --output-on-failure
cmake --build build-web --target HelloLoop-run
```

Because the suites link JSPI they need a node that has it:
`EACP_WEB_TEST_NODE` (cached, the node and its flags) is the first of the
emulator, `/opt/homebrew/bin/node` and `/usr/local/bin/node` that has
`WebAssembly.Suspending`. Node 25 and later have it, node 24 only behind
`--experimental-wasm-jspi`, which configure does not try, node 22 not at all;
with none found the configure warns, the test node is the emulator, and the
suites fail. Pass `-DEACP_WEB_TEST_NODE=<node>` to name one, and a node 24 as
a list with its flag, `-DEACP_WEB_TEST_NODE="<node>;--experimental-wasm-jspi"`.
An app links no JSPI unless it says `NESTED_PUMP`, so the ctest entries ask
nothing of the emulator's node beyond wasm exceptions, and `<app>-run` uses it
only for the launcher; a `NESTED_PUMP` app's launcher is the test node.

That node tier proves the client logic against a real server — statuses,
bodies, multipart, timeouts, cancellation, size limits, resume, the WebSocket
paths, the loop and the pool — but not a browser: node's `fetch()` applies no
CORS, sends any header and returns a redirect it was told not to follow. Nor
does it take the path a real app takes, since the suites link the nested pump.

The second tier is executables built as an app is, without JSPI, each
registered by `eacp_web_add_app_tests(<target>)` (`CMake/Web.cmake`) as two
ctest entries: `<target>`, `Launch.mjs node` under the emulator's plain node,
and `<target>Headless`, `Launch.mjs headless` in the Chrome configure found
into `EACP_WEB_CHROME` (Chrome or Chromium by their usual names and install
paths, Windows' Program Files and LocalAppData included), registered only
where it found one,
with a `STATUS` line saying so where it did not. An entry fails on the exit
code, on not exiting within 60 s, and on what the runtime prints when its exit
is aborted or something calls into it after it exited (`Aborted`, `aborted:`,
`Assertion failed`, `exitRuntime`, `after runtime exit`). Those messages come
from Emscripten's `ASSERTIONS`, which a Debug build has; a Release web build
prints none of them, so there the entries check little beyond the exit code
and the timeout. The node entries are the point: the path real apps take
is tested on a machine without Chrome. `Tests/CMakeLists.txt` registers these seven first:

- `HelloLoop` runs `callAsync`, `callAfter` and a `Timer` on the page's loop
  in order and quits.
- `HelloNetwork` talks to a test server of its own (its `COMPANION`). It
  checks that a blocking call is refused in an app, then runs GET, POST, a
  404, a timeout, followed and unfollowed redirects, a download read back,
  `fetchAsync` twice and a WebSocket echo and close, each started from the
  last one's callback, and prints what the browser did where browsers differ
  from node.
- `QuitWithTimerHarness` (`Tests/Core/QuitWithTimerHarness-Web.cpp`) quits
  with a static `Threads::Timer` alive, whose teardown during static
  destruction must not abort the exit.
- `QuitWithFetchHarness` (`Tests/Network/QuitWithFetchHarness-Web.cpp`)
  starts an `asyncRequest` the server answers only after 800 ms and opens a
  `WebSocket::Connection`; at 200 ms it sends a message, releases the
  connection so the socket stays open across the exit, and quits. Its server
  is a `COMPANION` `TestServer.mjs`, as `HelloNetwork`'s is, not the ctest
  fixture, and nothing may call into the exited runtime.
- `Console` (`Apps/Console/Console`), the desktop example unchanged: a 1 Hz
  `Timer` that logs four ticks and quits.
- `ZipDemo` (`Apps/Console/Zip`), unchanged: an archive built in memory,
  written to the in-memory file system, read back, extracted and deflated, in
  `main()` with no loop.
- `WebWindowHarness` (`Tests/Graphics/WebWindowHarness-Web.cpp`) opens a
  window with a recording view and a presenting child. In a page it checks
  the canvas (shown, its CSS and backing sizes at `devicePixelRatio`), then
  dispatches DOM events on it — a move, two quick presses counting 2, a drag
  and its delta, a right press, a pixel and a line wheel, a key kept (its
  default prevented, the polled state holding it) and released (passed on) —
  checks the CSS cursor follows the view's, `setSize`, the presenting view's
  `Kind::Web` surface (its selector, id and pixel size) and its one answered
  frame, three `DisplayLink` ticks, and that the window's canvas goes with it.
  Under node the same window must come up headless: no canvas, never shown,
  sized and resized, and the `DisplayLink` ticking on its timeout.

Each must exit 0, and none of them links `eacp-core-nested-pump`. Three more
do, the network examples that block in `main()`, unchanged on every platform:

- `OnlineResourceDemo` (`Apps/Console/OnlineResource`) fetches
  `<server>/robots.txt` through `OnlineResource::fetch` and prints it.
- `Http` (`Apps/Network/HTTP`) makes one blocking request of each kind — a
  JSON document, a query string, custom headers, a JSON and a form POST, a
  User-Agent, a bearer token, a download to a file and a multipart upload of
  it — and exits 1 if any failed.
- `BasicHttp` (`Apps/Network/BasicHTTP`) POSTs a reflected struct as JSON ten
  times and exits 1 if any failed.

Each takes its server as an optional positional argument (`Args::Parser`),
defaulting to `https://httpbingo.org`, an httpbin-compatible host that sends
CORS headers, answers preflights for the headers they set and takes ten
requests at once, so `<app>-run` and a native run need no server of their own.
Their ctest entries get `TestServer.mjs` as a `COMPANION`, set on the target by
`Tests/CMakeLists.txt` just before `eacp_web_add_app_tests` so `<app>-run`
keeps the public default; the server answers the httpbin endpoints they use
(`/json`, `/anything`, `/headers`, `/post`, `/user-agent`, `/ip`, `/bearer`,
and `/robots.txt` with an ETag) in httpbin's shapes, so the entries run
offline. There are no native ctest entries for them, since node, which the
server needs, is not a dependency of the native tests.

Last, `ThrowFromMainHarness` and `ThrowFromMainHarnessHeadless` run a
`NESTED_PUMP` harness that throws out of `main()` from inside a nested pump,
and pass on the `Aborted(uncaught exception: ...)` line within 20 s (see
"Uncaught exceptions").

The GPU runs in the browser alone. `Compute` and `AsyncCompute` are
WebGPU app entries (they link `eacp-gpu`): under node they say there is no device and exit 0, and
as `<app>Headless` they run their kernels on the page's device and fail on any
`WebGPU: ` line. `GPUTests` needs WebGPU throughout, so it is built as a page
(the nested pump, no node file system) and registered by
`eacp_web_discover_tests(GPUTests HEADLESS)`: its cases are listed through
node as every suite's are, and each group is an entry running
`Launch.mjs headless GPUTests.js --webgpu -- --test-group <group>`, a page of
its own; with no Chrome found it registers nothing. The whole suite in one page
takes about twenty seconds on an M4 Max. The cases a backend
feature the web lacks is behind fail rather than skip, and are listed in
`plan.md` (Phase 3, open items).

CI's web lane (`build-web` in `.github/workflows/build.yml`, Ubuntu,
`Debug`) runs all of this, `GPUTests` on SwiftShader (see "The GPU"). Its
`Show the WebGPU device` step runs `GPU/aDeviceIsPresentWhenRequired` in a
page and fails on `no WebGPU device`,
because `EACP_REQUIRE_GPU` cannot reach a page's environment. Three groups
fail on SwiftShader and pass on Metal, and the lane excludes them by name:
`Formats` makes an RG16Unorm texture the device has no
`texture-formats-tier1` for, which throws in the page and leaves it hanging
until the 300 s timeout, and `PackedHalf` (the four packing cases) and
`Intrinsics/errorFunctionIsOddAboutZero` get bit patterns from SwiftShader
that are not the exact ones they check for.

## Not there yet

- **Graphics.** Windows, views, input and the GPU are there, but no text, UI,
  SVG or 2D `Context`, so of `Apps/GPU` only the examples with no 2D overlay
  build (`plan.md` is the roadmap to the rest). No
  `TouchEvent`s (a finger is the left button), no `Magnify` from a pinch, no
  gamepads, image cursors are the arrow, and a presenting view smaller than
  its window still gets the whole canvas.
- **GPU gaps.** No occlusion queries, GPU timestamps, GPU mip generation,
  layered rendering, count-driven indirect draws, border-colour sampling,
  multisampled sampleable depth, storage binds off WebGPU's 256-byte offset
  grid, or worker-thread `Device`s; every `Device` shares the page's one
  device and queue. `CpuCompute` builds but starts a thread, which the web
  refuses.
- **Persistence.** The in-memory file system is all there is; nothing written
  in a page survives it.
- **Threads, opt-in.** A cross-origin isolated page could have them; the
  target has no `-pthread` mode, and `EACP_HAS_THREADS` is simply off.
- **Native surfaces of a page.** Clipboard, alerts, file pickers and opening
  files are the stubs above.
- **Browsers.** Headless Chrome, through the `<app>Headless` entries, is the
  only browser anything has run in, beside node; Firefox and Safari are
  untried. The nested pump has run in Chrome (`GPUTests`, the `NESTED_PUMP`
  GPU examples), and WebGPU on macOS's Metal adapter and Chrome's SwiftShader
  on Linux.
