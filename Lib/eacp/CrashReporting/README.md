# CrashReporting

When an app faults, a second process writes a report with the crashed
thread's stack, the app's metadata and its attachments, ends the app, and
hands the report to the app's own code. A main thread that stops answering
is reported the same way, with its stack, and the app left running. macOS and
Windows.

## Use

```cpp
struct MyCrashHandler : CrashReporting::CrashHandler
{
    // In the handler process, after the app is gone. May open windows.
    void crashReported(const CrashReporting::CrashReport& report) override;

    // In the handler process, while the hung app keeps running. Return
    // promptly: the handler watches the app again once this returns.
    void hangReported(const CrashReporting::CrashReport& report) override;

    // In the app, on the event loop after start() returns, for each report
    // still in pending/. Start long work here; never wait on it.
    void pendingReportFound(const CrashReporting::CrashReport& report) override;
};

EACP_REGISTER_CRASH_HANDLER(MyCrashHandler);

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    auto options = CrashReporting::Options {};
    options.environment = "production";
    options.attachments = {logPath};
    CrashReporting::start(options);

    CrashReporting::setTag("tier", "pro");
    CrashReporting::setExtra("build", "1234");
    CrashReporting::setUser({"42", "someone@example.com", "someone"});
    CrashReporting::addBreadcrumb({"ui", "opened settings"});
    ...
}
```

CMake: `eacp_enable_crash_reporting(<executable> [SYMBOLS_DIR <dir>])` links
`eacp-crash-reporting` and defines `EACP_CRASH_REPORTING=1`. It also makes
symbols every build: a `dsymutil` step writing `<target>.dSYM` beside the
executable or bundle on macOS (with `-g` added for Release and MinSizeRel), and
on Windows a PDB in every configuration (`ProgramDatabase` debug info unless
`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` says otherwise, `/DEBUG /OPT:REF
/OPT:ICF` for Release and MinSizeRel). That covers the target's own sources;
the libraries it links get debug info only as their own configuration gives
it. It refuses anything but an executable,
since the handler is that executable relaunched as
`<exe> --eacp-crash-handler <database> <channel> -- <Options::handlerArguments>`.

`Options`: `databaseDirectory` (default `appSupportDirectory()/Crashes`),
`maxHandledReports`, `handlerStartTimeout`, `channelName` (Windows object
names; default `createDefaultChannelName()`), `handlerArguments` (what
`Apps::commandLineArgs()` holds in the handler), `release` (default the
AppInfo version), `environment` and `dist` (into `metadata` under those keys)
and `attachments`, `heartbeatInterval` (500 ms) and `hangThreshold` (10 s,
zero turns hang detection off; see Hangs). `setTag`/`removeTag`, `setExtra`, `setUser`/`clearUser`,
`addBreadcrumb` (a ring of the newest 64, timestamped when added) and
`addAttachment` write into the shared block, so they cost nothing at crash
time. `stop()` ends the handler cleanly and restores the platform's own crash
handling; `start()` is refused after it. `crashedLastRun()` is whether a report
was written since the last `clearCrashedLastRun()` (a `crashed-last-run` marker
in the database). `isHandlerRunning()` says
whether a crash right now would be caught. `markReportHandled(id)` moves a
report from `pending/` to `handled/` and prunes the latter.
`pauseHangDetection()`/`resumeHangDetection()` bracket main-thread work that
is meant to take longer than the threshold; they nest.

## Report

```
<database>/pending/<id>/report.json
                       minidump.dmp       Windows only
                       attachments/<copies>
<database>/handled/<id>/...
<database>/crashed-last-run
```

`report.json` is written last, so a folder without one is a capture that did
not finish; it is read back as `reason: "incomplete"`. Fields: `id` (UUID),
`kind` (`crash` or `hang`; a report without it reads back as a crash),
`timestamp` (ms since epoch), `reason`, `message`, `faultAddress`,
`crashingThread {threadId, frames[{modulePath, moduleBase, offset, symbol,
moduleUuid}]}`, `hangDuration` (ms, 0 for a crash),
`minidumpPath`, `metadata` (with `appName`, `appVersion`, `os`, `arch` —
`arm64` or `x86_64` on macOS, `arm64` or `x64` on Windows, the app's own build
— `release`, `environment`, `dist`), `tags`, `extra`, `user {id, email, username}`,
`breadcrumbs[{category, message, level, timestamp}]` (oldest first),
`attachments`, `platform`, `capturedBy`
(`handler`, `exit-code`, `incomplete`). Addresses are hex strings, since a
JSON number loses the low bits of a 64-bit address.

`moduleUuid` is the key a symbol server finds the image's symbols by. On macOS
it is the Mach-O `LC_UUID`, upper-case `8-4-4-4-12` as `dwarfdump --uuid`
prints it; on Windows the PDB's CodeView GUID and age,
lower-case `8-4-4-4-12-<age hex>`. Empty where it was not read; a report
written before the field existed reads back with it empty.

## Reading reports elsewhere

The report types and the database are their own target, `eacp-crash-report`,
built on every platform, Linux included, linking only `eacp-core`. A server or
tool links it and reads the exact `report.json` an app wrote into the same
`CrashReport` struct with `CrashReport::fromJSON`;
`eacp-crash-reporting`, the macOS and Windows capture half, links it PUBLIC.

## Writing an uploader

The module uploads nothing; an app does it in its own `CrashHandler`.

- Upload from `crashReported`, in the handler process: it is healthy, the app
  is already gone, and it may take as long as it needs.
- Resend from `pendingReportFound` on every launch, for reports that never
  arrived, and `markReportHandled(id)` once the server has taken one. It runs
  on the event loop, so start the upload asynchronously; never wait on it.
- The report folder is the payload: `report.json`, `minidump.dmp` when there
  is one, and the copies under `attachments/`.
- The report `id` is the idempotency key. Resends are expected, so a server
  should dedupe on it.
- Symbols change with every build, as `moduleUuid` does, so keep each build's
  with it: `eacp_enable_crash_reporting(<target> SYMBOLS_DIR <dir>)` copies
  the dSYM or PDB there as `<target>-<version>`.
- The handler has no app state. Pass what it needs (an endpoint, a key)
  through `Options::handlerArguments`, read back with
  `Apps::commandLineArgs()`, or as a tag, which arrives in `report.tags`.

The fields map onto what crash backends such as Sentry expect.

## What is caught

| Fault | macOS | Windows |
|---|---|---|
| Null dereference, bad access | `SIGSEGV`/`SIGBUS`, stack | `EXCEPTION_ACCESS_VIOLATION`, stack, minidump |
| Stack overflow (thread that called `start()`) | `SIGSEGV`, stack | `EXCEPTION_STACK_OVERFLOW`, stack, minidump |
| `abort()` | `SIGABRT`, stack | `SIGABRT`, stack, minidump |
| Pure virtual call | `SIGABRT`, stack | `purecall`, stack, minidump |
| Uncaught C++ exception | `std::terminate`, type and `what()`, stack | `std::terminate`, type and `what()`, stack, minidump |
| Uncaught `NSException` | `NSException`, name and reason, stack | — |
| Heap corruption | `SIGTRAP`/`SIGABRT` from malloc's checks, stack | `STATUS_HEAP_CORRUPTION` from the exit code, no stack |
| `__fastfail` | — | `STATUS_STACK_BUFFER_OVERRUN` from the exit code, no stack |
| Illegal instruction, FP, trap, `SIGSYS` | signal name, stack | exception name, stack, minidump |
| CRT invalid parameter | — | `invalid parameter`, stack, minidump |

Not caught: `SIGKILL`, the OOM killer, a crash while the handler is
not running (it is re-raised and lost), and on Windows anything that bypasses
the unhandled-exception filter other than through the exit code above.

| Main thread hang | `Hang`, main thread's stack, app keeps running | `Hang`, main thread's stack, minidump, app keeps running |

## Hangs

`start()` arms a heartbeat: a callback on the event loop, re-posted with
`Threads::callAfter` each time it runs, writes the steady clock into the
shared block every `heartbeatInterval`. (Not a `Threads::Timer`: Windows
delays `WM_TIMER` behind other messages and coalesces it by up to a second.) When the
heartbeat has been silent for `hangThreshold` past the beat that was due, the main thread (the thread
that called `start()`) is sampled and a report is written to `pending/` like a
crash's: `kind: "hang"`, `reason: "Hang"`, `message: "main thread
unresponsive for N s"`, `hangDuration`, the main thread's frames in
`crashingThread`, and the tags, extra, user, breadcrumbs, attachments and
metadata as for a crash. The app is not ended, and `crashedLastRun()` is not
set. `hangReported` is called in the handler process; `pendingReportFound`
delivers hang reports on later launches too. One report per episode: the next
is armed once the heartbeat beats again.

Nothing is watched until the first beat, so an app that never runs the event
loop (a console tool) is never reported as hung. Detection is off while
`pauseHangDetection()` is in effect, and the silence is measured afresh from
`resumeHangDetection()`. It is off while a debugger is attached, so a
breakpoint is not a hang.

- **macOS.** A watchdog thread in the app checks the heartbeat. On a stall it
  `thread_suspend`s the main thread, reads its registers with
  `thread_get_state` (`ARM_THREAD_STATE64` or `x86_THREAD_STATE64`), walks
  the frame-pointer chain from pc, lr and fp inside the thread's stack bounds
  (pointer-authentication bits stripped on arm64e), `thread_resume`s it, and
  writes the addresses into the shared block's hang record and a hang byte to
  the handler's pipe. The handler symbolises them as it does a crash's. The
  debugger check is `P_TRACED`.
- **Windows.** The handler watches the heartbeat from its own wait loop, so a
  hang is caught even if the app's threads all stall. It opens the main
  thread, `SuspendThread`s it, takes `GetThreadContext`, walks it with
  `StackWalk64`, `ResumeThread`s it, then names the frames and writes a
  minidump of the threads as they are. On ARM64 it follows the x29
  frame-pointer chain instead of `StackWalk64`, which stops at the first
  ntdll frame of a thread parked in a wait; functions built without frame
  records (clang-cl's optimised ARM64 code) are skipped. The debugger check is
  `CheckRemoteDebuggerPresent`.

Limits: only the main thread is sampled, once, at the threshold; there is no
deadlock analysis or other threads' stacks. A frame-pointer walk on macOS
loses frames compiled without frame pointers. While `hangReported` runs, the
handler is not watching for a crash; one that happens then is handled once it
returns.

## How it works, and the v1 limits

**macOS.** The app captures its own crash: `start()` primes `backtrace()`,
installs an alternate signal stack, records every loaded image (and dyld)
into the shared block, and installs `sigaction` handlers, `std::set_terminate`
and `NSSetUncaughtExceptionHandler`. The signal handler records the signal,
the faulting PC and `backtrace()`, writes one byte to the handler's pipe and
waits to be killed. The handler, started with `posix_spawn` holding the shared
block (an unlinked temp file) and two pipes, maps each frame to module and
offset, symbolises it with `dladdr` against its own copy of the same image,
writes the report, sends the app `SIGKILL` and calls `crashReported`. EOF on
the pipe with no byte means the app exited cleanly.

- The capture is in-process with `backtrace()`; no Mach exception ports and
  no out-of-process stack walk.
- No minidump on macOS.
- The alternate stack is the starting thread's only, so a stack overflow on
  another thread is not reported.
- The handler process keeps the app's executable name; macOS has no clean way
  to retitle it.

**Windows.** The app shares a named file mapping and two named events with the
handler, which holds a process handle on the app from startup.
`SetUnhandledExceptionFilter`, `_set_invalid_parameter_handler`,
`_set_purecall_handler`, `signal(SIGABRT)` and `std::set_terminate` record the
reason and `EXCEPTION_POINTERS` address and signal the handler, which writes a
minidump (`MiniDumpWriteDump`, `ClientPointers = TRUE`), walks the thread with
`StackWalk64` and `SymFromAddrW`, terminates the app and calls
`crashReported`. An uncaught C++ exception reaches the filter as a structured
exception, so its type and `what()` are read from the MSVC exception record.
The app calls `AllowSetForegroundWindow` on the handler as it launches it, so
a window `crashReported` opens can take the foreground with `Window::toFront()`.

- No WER runtime-exception module, so `__fastfail` and heap corruption are
  reported from the exit code only, with no stack.
- No vectored exception handlers.
- On ARM64, `StackWalk64` stops an uncaught C++ exception's stack at
  KERNELBASE's `RaiseException`, so that report has one frame; the minidump
  has the whole thread.

Both: when the handler process is gone at crash time, the crash is not
reported; there is no in-process fallback in v1.
