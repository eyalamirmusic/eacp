# The web target

The web target is eacp compiled to wasm32 with Emscripten, running in a
browser page or under node. So far it is `Core` and `Network` and the
device-free pieces that build everywhere — `eacp-gpu-codegen`,
`eacp-cpu-compute`, `eacp-ml-graph` and `eacp-webview-bridge` — and nothing
that draws: the seven drawing capability variables are all off there (see
[Build.md](Build.md)), so `Graphics`, `GPU`, `Text`, `UI` and everything above
them are not built. This page is the account of what the target is, how it is
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
what builds is what needs nothing that draws and no socket of its own: the two
`Apps/Web` examples written for the browser's loop, `HelloLoop` and
`HelloNetwork`, and five examples of every platform, unchanged — `Console`,
`ZipDemo` and `OnlineResourceDemo` from `Apps/Console`, `Http` and `BasicHttp`
from `Apps/Network`. The servers, TCP and IPC examples are behind
`EACP_HAS_SOCKETS` and `Apps/Plugins` is never added. An app is an ordinary
`eacp_add_app` or `add_executable`, which on the web produces `<app>.js` and
`<app>.wasm`, plus one call:

```cmake
eacp_add_app(HelloLoop Main.cpp)
target_link_libraries(HelloLoop PRIVATE eacp-core)
eacp_web_executable(HelloLoop)
```

`eacp_web_executable(<target> [NODE_FILESYSTEM] [NESTED_PUMP]
[COMPANION <script.mjs>] [ARGUMENTS <arg>...])` gives it the link options every
web executable shares (`eacp_web_link_options`): `-sALLOW_MEMORY_GROWTH=1`, a
1 MB stack (`-sSTACK_SIZE=1MB`) and `-sEXIT_RUNTIME=1`, so the runtime exits
with the code `Apps::quit` or `main()` gives it, and `CMake/Web/UncaughtException.js`
as a `--post-js` (below, "Uncaught exceptions"). `NODE_FILESYSTEM` adds
`-sNODERAWFS=1`, node's own file system under the program, which makes the
executable node-only; the test suites link that way, an app does not.
`NESTED_PUMP` links `eacp-core-nested-pump` (see "The nested pump"), which is
how an example that blocks in `main()` — `OnlineResourceDemo`, `Http`,
`BasicHttp` — runs unchanged, at the price of needing JSPI. No HTML page is
produced: the build output is the `.js` and the `.wasm` beside it, and a page
that defines `Module` and loads the `.js` is the deployer's.

An executable that can run in a browser also gets one launcher target,
`<app>-run`, not part of `all`: it builds the app, serves it on a loopback
port and opens it in the default browser. It is `CMake/Web/Launch.mjs`, plain
node with no packages, in its `browser` mode; the same script has a `node`
mode, which runs the app under node, and a `headless` mode, which does what
`browser` does in the headless Chrome named by `--chrome` (the ctest entries
pass `EACP_WEB_CHROME`), failing with 124 if the app has not exited within
120 s, and at once with no Chrome. No target runs those two; the app's ctest
entries do (see "Testing"), and by hand, from the app's build directory, they
are

```bash
node CMake/Web/Launch.mjs node <app>.js [--companion <script.mjs>] [-- <args>...]
node CMake/Web/Launch.mjs headless <app>.js --chrome <chrome> [-- <args>...]
```

with `CMake/Web/Launch.mjs` given by its path in the source tree. In the
browser modes it serves the app's directory as static files and a generated
page that hands the program its arguments through `Module.arguments`, relays
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
which are read when a launch command is made: by `eacp_web_executable` itself
for `<app>-run`, and by `eacp_web_add_app_tests` for the ctest entries
(see "Testing"). They are therefore given to `eacp_web_executable`; a property
set on the target afterwards reaches only an `eacp_web_add_app_tests` called
after that.

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
`eacp_web_executable(<app> NESTED_PUMP)`; its launcher and ctest entries then
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
- `Processes::runAsync`, `BuildRunner` and `Rpc`'s worker-thread commands run
  their job on a later turn of the loop through `callAsync`. For the first two
  that job is a launch, which fails on the web (below).
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

`Files::executablePath()` is the `.js` beside the `.wasm` (`Utils/Files-Web.cpp`):
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
`CpuComputeTests`, `MLGraphTests` and `ScriptHostTests` for the web, each
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
writes.

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
is tested on a machine without Chrome. `Tests/CMakeLists.txt` registers these six first:

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
"Uncaught exceptions"). These are a few apps in one browser; nothing else runs
in a browser automatically.

There is no web lane in CI: `.github/workflows/build.yml` neither installs
Emscripten nor builds the target.

## Not there yet

- **Graphics.** No window, view, input, GPU, text, UI or SVG: every drawing
  capability variable is off, and `Apps/` builds only the seven examples that
  draw nothing (`plan.md` is the roadmap to the rest). The shader codegen builds, and on the web takes the Linux builder,
  so a `ShaderBuilder` emits GLSL for the Vulkan backend there.
- **Persistence.** The in-memory file system is all there is; nothing written
  in a page survives it.
- **Threads, opt-in.** A cross-origin isolated page could have them; the
  target has no `-pthread` mode, and `EACP_HAS_THREADS` is simply off.
- **Native surfaces of a page.** Clipboard, alerts, file pickers and opening
  files are the stubs above.
- **Browsers.** Headless Chrome, through the `<app>Headless` entries, is the
  only browser anything has run in, beside node; Firefox and Safari are
  untried, and so is the nested pump in any browser.
- **CI.** No lane builds or runs it.
