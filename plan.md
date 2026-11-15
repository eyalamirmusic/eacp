# Web (WebAssembly) target: state of the work

Branch `web-target`, updated 2026-11-14. **Nothing is committed or pushed**:
every change below is in the working tree.

The first wave and every item of the review are in the tree. `build-web` is
fresh, built and passing (107 of 107), and the final tree passes the CI
configure on macOS, Windows x64 and Linux aarch64. What is left is the user's
decisions listed below and the commit.

## Direction

The first attempt at the web target made pthreads mandatory (so every page had
to be served cross-origin isolated) in order to keep eacp's blocking APIs and
desktop tests running unchanged. That was reviewed and replaced with a
**browser-first** design:

- single-threaded, no `-pthread`; output is plain static files, no special headers
- the browser's event loop is eacp's loop
- networking is async-first on `fetch()` and the browser `WebSocket`
- scope so far is `Core` and `Network` plus the device-free pieces; nothing draws

The original thread-based work is kept at `.git/web-threads-wip-backup/`
(`tracked.patch` and `untracked.tar.gz`).

## Done and verified

### Core and build

- `CMake/Web.cmake`: no pthread flags, pool sizes or isolation requirement.
- `Lib/eacp/Core/Threads/EventLoop-Web.cpp`: `callAsync` is a plain queue drained
  on a later browser turn; `callAfter` is one plain JS `setTimeout` for the
  nearest deadline, holding no Emscripten keepalive and disarmed at runtime
  exit; `Timer-Web.cpp` is a plain JS `setInterval` through `EM_JS`, also with
  no keepalive. `run()` hands control back to the browser, `quit()` tears down.
- `run()` unwinds the stack: with `-fwasm-exceptions` the destructors of the
  caller's locals run before the app starts, and a `catch (...)` around `run()`
  does not stop it. Kept, and stated in `App.h`, `App.cpp`, the top of
  `EventLoop-Web.cpp` and `Docs/Web.md`.
- Nested pump (JSPI) is per executable: the OBJECT library `eacp-core-nested-pump`
  (`Threads/NestedPump-Web.cpp`) is linked by test suites only. Apps contain no JSPI.
  A quit pending when a nested pump starts is saved and cleared, the pump waits
  normally, and the quit is restored on return (`EventLoop::runFor`), as on
  Linux and macOS. Blocking HTTP, `OnlineResource::fetch`, `Async::waitFor` and
  `runEventLoopUntil` after `quit()` therefore complete instead of hanging.
- `attachCurrentThreadAsMain` is a no-op and detach matches
  `EventLoop-Default.cpp`. Nothing on the web calls them.
- New capabilities: `EACP_HAS_THREADS` and `Platform::hasThreads()`, false on web.
  `Thread::start()` fails honestly there; `ThreadPool` runs one task per loop turn.
- Zero-thread `ThreadPool`: a `TaskGroup`'s tag is a `shared_ptr` held by the
  group, its queued tasks and completions, and a closing group queues no
  completion; a task running on the main thread counts as a pool thread, so
  the native asserts on waits from a task apply; `stop()` resets
  `mainThreadRunPending`.
- Timer ticks coalesce (`tickQueued`). `Timer-Linux.cpp` does not coalesce
  either and was left alone.
- `Process-Default.cpp` is the one unsupported `Process` source for iOS and the
  web (`Process-iOS.cpp` and `Process-Web.cpp` deleted): `launched()` false.
- `eacp_add_plugin` on the web is a configure `FATAL_ERROR`. The hot-reload
  sources stay in the web `eacp-core`, since static-mode `Plugins::Reloadable`
  needs them, and `eacp_add_hot_reload` is always static there.
- `Apps/Web/HelloLoop` replaces `HelloThreads`.

### Changes on every platform

- `Apps::run` (`run<T>` in `App.h`, `run(func)` in `App.cpp`) calls
  `Threads::stopCallAfterScheduler()` after `stopSharedThreadPool()`: pending
  `callAfter` callbacks are dropped and the scheduler thread is joined when
  `run` returns.
- The WebSocket close timeout is a `callAfter` (no thread); parallel chunked
  downloads moved to `HTTP/HttpParallel.cpp`.
- `TCP/Connection-Windows.cpp` includes `WinInclude.h` first. Moving Network
  sources into the `EACP_HAS_THREADS`/`EACP_HAS_SOCKETS` blocks shifted the
  unity batches and put its bare `<winsock2.h>` (no `NOMINMAX`) ahead of
  `WebSocket-Windows.cpp`, breaking the Windows CI build. It was the only such
  source.
- `threadReportNoThreads` in `Thread.cpp` is `[[maybe_unused]]`
  (`-Wunneeded-internal-declaration`).

### Network

- `HTTP/Http-Web.cpp` + `Http-Web.js`: push-style `fetch` driver. `HttpAsync-Web.cpp`
  is the async client. Blocking calls return a clear error unless the executable
  links the nested pump.
- `OnlineResource`: fetch split around its download behind one seam
  (`OnlineResourceInternal.h`), with `-Threaded.cpp` (native) and `-Web.cpp`.
- `WebSocket/WebSocket-Web.cpp` + `.js`: real backend over the browser `WebSocket`.
- At runtime exit, `addOnExit` callbacks abort every live fetch
  (`EacpWebHttp.abortAll`) and release every socket
  (`EacpWebSocket.releaseAll`), so nothing calls into a torn-down runtime after
  `quit()`.
- New capability `EACP_HAS_SOCKETS`, false on web: servers, TCP and IPC are not built.
- `Http.h`/`Http.cpp` say what browsers do with an app-set User-Agent (Chrome
  dropping it was observed; Firefox sending it, which forces a CORS preflight,
  is from the review and was not run). `WebSocket.h` says invalid UTF-8 in text
  goes out as U+FFFD.

### Tests

- `eacp_web_add_app_tests(<target>)` (`CMake/Web.cmake`) registers `<target>`
  under the emulator node and `<target>Headless` when configure found a Chrome,
  for `HelloLoop`, `HelloNetwork`, `QuitWithTimerHarness` and
  `QuitWithFetchHarness`. Each entry has a 60 s timeout and fails on
  Emscripten's abort and after-exit messages, which only a Debug (`ASSERTIONS`)
  build prints. Configure prints a `STATUS` line when no Chrome is found.
- Real-exit harnesses, neither linking the nested pump:
  `Tests/Core/QuitWithTimerHarness-Web.cpp` quits with a static `Timer` alive;
  `Tests/Network/QuitWithFetchHarness-Web.cpp` quits with an `asyncRequest` and
  an open `WebSocket::Connection` in flight (own companion test server; exits 1
  if the request answered or the socket had not opened before the quit). All
  four entries fail without their fixes. 20 of 20 under node each, 5 of 5
  headless each.
- New cases, each failing without its fix: `Threadless/aGroupDestroyedByItsOwnTaskDeliversNothing`
  (the freed-memory half shows only if the allocator reuses the block),
  `Threadless/aRunningTaskCountsAsAPoolThread`,
  `WebHttp/aBlockingRequestAfterQuitStillCompletes`,
  `Timer/ticksNoLoopDrainedRunOnce` (web only, reached through the internal
  suspend hook), and on every platform `App/runDropsTheCallAftersItLeft` and
  `App/runOfAnAppDropsTheCallAftersItLeft`.
  `WebHttp/theRuntimeExitingSilencesATransfer` and
  `WebWebSocket/theRuntimeExitingSilencesASocket` run the exit callback list,
  not a real exit; the harnesses cover the real one.
- The `maxRedirects = 3` case is now `WebHttpRedirect/aPositiveLimitIsTheRuntimesTwenty`;
  the User-Agent case is `WebHttp/defaultAndOwnUserAgentsReachTheServerUnderNode`.
- `DynamicLibraryTests`, `HotReloadLoopTests` and `ReloadableTests` are not
  built on the web; those files and `Tests/Core/Common.h` are back to HEAD.
- Test discovery (`CMake/Web/DiscoverTests.cmake`, the generated
  `CTestCustom.cmake`) is kept. Without it the entries collapse to about six
  whole-suite ones, `ctest -R <group>` and parallel runs go, "listed no cases"
  stops failing, and one wasm trap takes down every later case in its suite
  (observed). The clean fix is upstream: NanoTest running its POST_BUILD
  discovery through `CMAKE_CROSSCOMPILING_EMULATOR`.

### Build plumbing

- Node: `EACP_WEB_TEST_NODE` is the first of the emulator,
  `/opt/homebrew/bin/node` and `/usr/local/bin/node` that has JSPI; there is no
  automatic `--experimental-wasm-jspi` retry. `EACP_WEB_NODE` and
  `EACP_WEB_HOMEBREW_NODE` are gone.
- `CMake/Web/Launch.mjs`: `--timeout`, its own Chrome search and `$EACP_CHROME`
  are gone; Chrome comes from `EACP_WEB_CHROME`. It relays for 1 s after a page
  reports its exit, adding about a second to every browser-mode run.
- `-sNODERAWFS` and the other node link options are per target in
  `Tests/CMakeLists.txt`, not directory-wide.
- `Http-Web.cpp` cleanups, non-null `std::function` defaults, comments cut back,
  stale comments in `CallAfter.h` and `ThreadPool.h` fixed.

### Results

| Check | Result |
|---|---|
| Web `ctest`, fresh `build-web` (test node 26.13.1, emulator node 22.22.0) | 107 of 107, three runs, then once more after the last follow-ups; 1269 cases |
| Web app ctest entries, node and headless Chrome, of `HelloLoop`, `HelloNetwork`, `QuitWithTimerHarness`, `QuitWithFetchHarness` | 8 of 8 |
| `HelloLoop-run` (`Launch.mjs browser`, the one launch target per app): page, `.js` and `.wasm` served (200, right types), the opened default-browser tab's output relayed, launcher and server gone after | exit 0, `PASS` |
| `CpuComputeBench` under node | exit 0 |
| Native macOS, fresh tree, whole build and whole `ctest` | 375 of 375 (one run); `NetworkTests` 413 cases; `App`, `EventLoop`, `Thread`, `ThreadPool`, `Timer` rerun after the last follow-ups: pass |
| Unity compile (`-DEACP_CI_BUILD=ON`) | native `eacp-core`, `eacp-network`, `CoreTests`, `NetworkTests` clean; whole web tree clean |
| clang-format | all 65 changed C++ files clean at the time of the check |
| iOS simulator, Ninja (not CI's Xcode generator) | `eacp-core` builds, including `Process-Default.cpp`; nothing else built |
| Windows x64, MSVC 14.52.37025, final tree, CI configure | build clean, no warnings from this branch; `ctest` 372 of 372 in the desktop session; the two new `App/` cases 10 of 10 runs of the `App` group |
| Windows clang-cl 22.1.3, final tree, CI configure | `eacp-core`, `eacp-network`, `CoreTests`, `NetworkTests` build clean (`threadReportNoThreads` warning gone); test binaries not run |
| Linux aarch64 (Fedora Asahi), GCC 16.2.1 and Clang 23.1.5, final tree, CI configure | both build clean, 0 warnings; headless (real GPU) 333 of 334 each; the two `App/` cases 10 of 10 runs on each |
| Linux on lavapipe as CI runs it, GCC | Weston 332 of 332, Xvfb 3 of 3, `GPUTests` with `EACP_VK_RENDER_PASSES=1` 818 of 818 |

The web runner prints nothing for a skip: 13 cases self-skip, all in
`CoreTests`, counted from source.

The final Windows and Linux runs are from a fresh copy of the final tree with
every build tree configured fresh, on CI's exact configure
(`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DEACP_CI_BUILD=ON`, unity on).

The Linux headless failure, `CommandBufferTiming/sharesSumToTheBuffer`, predates
this branch: it fails the same way, under GCC and Clang, with the `GPUTests` of
that machine's existing checkout (`257563be`), and passes on lavapipe in both
trees. It is the real GPU (Apple M1 Max, Honeykrisp driver), not this work.

### Documentation

`Docs/Web.md`, `Docs/Build.md`, `Docs/HotReload.md`, `Docs/Toolbox.md` and
`CLAUDE.md` describe the tree as it stands, including `run()` and the
User-Agent.

## Decisions to confirm

Made during the fix pass; each can be reversed.

- iOS no longer asserts on a process launch attempt in debug builds; the shared
  `Process-Default.cpp` reports `launched()` false.
- `eacp_add_plugin` on the web is a `FATAL_ERROR`.
- Test discovery is kept (see Tests).
- The hot-reload sources stay in the web `eacp-core`.
- No automatic JSPI-flag retry for node 24; it must be named as a list.
- A blocking call after `quit()` in a nested pump completes (loop-level fix)
  rather than returning an error.
- `Apps::run` stops the `callAfter` scheduler on every platform.

## Known and left alone

- Web: `TaskGroup::wait()` from a task can still return early when the group's
  remaining task is further up the same stack.
- Native: destroying a group from its own task or callback deadlocks in
  `closeGroup`, as before this work.
- `ThreadPool::stop()` resetting `mainThreadRunPending` and the hosted path
  have no test: reaching the first needs `detachCurrentThreadAsMain`, which
  would break the harness's loop.
- A pending `callAfter` no longer keeps node alive after `main()` returns
  (4.13 s before, 0.12 s after); no permanent test.

## Next steps

1. Confirm or reverse the decisions above.
2. Decide when to commit. Nothing has been committed or pushed.
3. Later: `README.md` platform table, a web CI lane, the upstream NanoTest
   discovery fix, then graphics (canvas, input, WebGPU with a WGSL emitter,
   text), persistent storage, opt-in threads.

## Not yet verified

- The Windows ARM64 lanes (no ARM64 MSVC cross compiler on the Windows box)
  and the iOS and Android CI lanes with this tree.
- Running the clang-cl test binaries; they were built only.
- The Linux display passes (Weston, Xvfb) with the Clang build; only GCC ran them.
- Firefox and Safari: untried. Only Chrome has run any of this in a browser:
  headless for the ctest entries, and the desktop Chrome (this Mac's default
  browser) for `HelloLoop-run`.
- In a browser, only the four app entries ran. Timeouts other than the
  whole-request one, cancel, the response size limit, resume, zip unpacking and
  the WebSocket failure paths pass under node only.
- Release web builds: the after-exit test patterns print nothing there.
- A node 24 list passed as `EACP_WEB_TEST_NODE`.
- The no-Chrome `STATUS` path in a real configure; the Chrome search on
  Windows and Linux hosts; `Launch.mjs` `browser` mode (`<app>-run`) on
  Windows and Linux, and for any app but `HelloLoop`.
- Native `ctest` stability beyond one full run.
- There is no web CI lane.

## Building and running

```bash
cmake -G Ninja -B build-web -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DCMAKE_TOOLCHAIN_FILE=/opt/homebrew/opt/emscripten/libexec/cmake/Modules/Platform/Emscripten.cmake
cmake --build build-web -j 10
ctest --test-dir build-web -j 6
cmake --build build-web --target HelloLoop-run
```

- `<app>-run` is the one launch target per web app (`HelloLoop`,
  `HelloNetwork`, `QuitWithTimerHarness`, `QuitWithFetchHarness`): it opens
  the app in the default browser and exits with its code once the page
  reports it. Under node or headless Chrome an app runs through its ctest
  entry (`ctest --test-dir build-web -R '^HelloLoop$'`, `-R
  '^HelloLoopHeadless$'`) or by hand from its build directory:
  `node <src>/CMake/Web/Launch.mjs node HelloLoop.js`, or
  `... headless HelloLoop.js --chrome <chrome>`, with
  `--companion <script.mjs>` and `-- <args>...` as the app needs.

- The apps run under the emulator, the node on `PATH` (v22 here), which is
  enough for them. The test suites need JSPI and run under
  `EACP_WEB_TEST_NODE`: Homebrew's node 26 here. Node 24 needs
  `-DEACP_WEB_TEST_NODE="<node>;--experimental-wasm-jspi"`.
- Build trees, all ignored by git: `build-web` (fresh, built, passing),
  `build-network`, `build-native-check`, `cmake-build-debug`, and
  `cmake-build-web` (the IDE's).
