# CrashReporting in depth

The whole account behind `Lib/eacp/CrashReporting/README.md`: the options,
the report format, uploading, every fault, the zoo, and how capture works on
each platform.

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
(`handler`, `exit-code`, `app`, `incomplete`), and `terminateFrames`, which is
empty unless `crashingThread` holds an uncaught exception's throw site (see
Uncaught C++ exceptions). Addresses are hex strings, since a
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

`CrashReport.h` declares the Miro reflection for `StackFrame`,
`CrashedThread`, `User`, `Breadcrumb` and `CrashReport` (free `reflect`
overloads, found by argument-dependent lookup), the same definition
`report.json` is written and read with. A consumer serialises any of them with
`Miro::toJSON` and gets the report's own field names, hex-string addresses and
enum names, without a reflection of its own.

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

Each row is a fault in the crash zoo (below), raised on the main thread and on
a background thread. [`MATRIX.md`](../Lib/eacp/CrashReporting/MATRIX.md) is the per-lane version, generated
from `Zoo/Matrix.cpp`, the table `Tests/CrashReporting` runs every cell of
(`CrashMatrix/<fault>/<thread>`) and checks the file against; change the
table, then `SelfDestruct --matrix Lib/eacp/CrashReporting/MATRIX.md`.

| Fault | Thread | macOS | Windows |
|---|---|---|---|
| Null dereference | main | `SIGSEGV`, stack | `EXCEPTION_ACCESS_VIOLATION`, stack, minidump |
| | background | `SIGSEGV`, stack | `EXCEPTION_ACCESS_VIOLATION`, stack, minidump |
| Integer divide by zero | main | — (arm64 does not trap); `SIGFPE` on x86_64 | `EXCEPTION_INT_DIVIDE_BY_ZERO`, stack, minidump (x64, MSVC ARM64; clang-cl ARM64 does not trap) |
| | background | as main | as main |
| Stack overflow | main | `SIGSEGV`, stack | `EXCEPTION_STACK_OVERFLOW`, stack, minidump |
| | background | `SIGBUS`, stack (an eacp thread, or one holding an `AlternateSignalStack`) | `EXCEPTION_STACK_OVERFLOW`, stack, minidump |
| `abort()` | both | `SIGABRT`, stack | `SIGABRT`, stack, minidump |
| Pure virtual call | both | `SIGABRT`, stack | `purecall`, stack, minidump |
| Uncaught C++ exception | both | `std::terminate`, type and `what()`, throw-site stack, `terminateFrames` | `std::terminate`, type and `what()`, stack through the throw, minidump |
| Heap corruption | main | `SIGTRAP` from malloc's checks, stack | `STATUS_HEAP_CORRUPTION` from the exit code, no stack |
| | background | `SIGABRT` (`SIGTRAP` on some runs) from malloc's checks, stack | `STATUS_HEAP_CORRUPTION` from the exit code, no stack |
| `__fastfail` | both | — | `STATUS_STACK_BUFFER_OVERRUN` from the exit code, no stack |
| Hang, spin, deadlock | main | `Hang`, main thread's stack, app keeps running | `Hang`, main thread's stack, minidump, app keeps running |
| | background | not reported: only the main thread is watched | not reported |
| Uncaught `NSException` | main | `NSException`, name and reason, stack | — |
| Illegal instruction, FP, trap, `SIGSYS` | any | signal name, stack | exception name, stack, minidump |
| CRT invalid parameter | any | — | `invalid parameter`, stack, minidump |

A background stack overflow is `SIGBUS` on macOS because a secondary thread's
guard page is a protection fault; its frames are all the recursion. A
background heap corruption is `SIGABRT` because another thread's small blocks
come from the zone allocator rather than the main thread's fast path. With clang-cl,
an exception escaping a background `std::thread` terminates with none current,
so Windows reports it as the `SIGABRT` that follows, with no type or `what()`;
MSVC's reports `std::terminate` (the CRT's terminate handler is per thread, so
the abort handler reads the exception still current). On Windows ARM64,
`StackWalk64` stops a background thread's `abort` or purecall inside the CRT.

Not caught: `SIGKILL`, the OOM killer, a stack overflow on a thread with no
alternate stack (macOS: one eacp did not create and that holds no
`AlternateSignalStack`), and on Windows anything that bypasses the
unhandled-exception filter other than through the exit code above. A crash
while the handler is gone is reported by the app itself (Fallback report).

## The crash zoo

`eacp-crash-zoo` (`Zoo/Zoo.h`, `eacp::CrashReporting::Zoo`, built where
`eacp-crash-reporting` is) is the list of deliberate faults above, for demo
and test apps: `listFaults()` (name, label, description, `stallsItsThread`,
`isAvailable` on this platform), `findFault(name)`,
`triggerFault(name, ThreadChoice)` (`main` runs it on the calling thread,
`background` on a fresh `Threads::Thread` that is never joined),
`scheduleFault(name, ThreadChoice, delay)` from an event-loop callback,
`setHangDuration`, and `killHandlerProcess()`, which ends the handler so the
next crash exercises the fallback report. The deadlock fault locks two mutexes
against a second thread in the opposite order. `Apps/CrashReporting/SelfDestruct`
shows each fault as a Main thread and a Background button, and
`SelfDestruct --crash <name> --thread main|background [--kill-handler]` raises
one with no window.

## Alternate signal stacks

A signal handler for a stack overflow needs a stack to run on.
`Threads::AlternateSignalStack` (Core, `Threads/AlternateSignalStack.h`)
installs a `sigaltstack` for the calling thread for its lifetime (64 KiB by
default) and removes it at the end; a thread that already has one keeps it. On
Windows it is a `SetThreadStackGuarantee` reserve, the same thing for the
exception filter. Every `Threads::Thread` holds one, and `start()` installs a
256 KiB one (64 KiB reserve on Windows) on the thread that called it, so a
stack overflow on any eacp thread is reported. A thread eacp did not create
(a `std::thread`, a host's thread) makes one at its start:

```cpp
auto worker = std::thread {[]
{
    auto signalStack = Threads::AlternateSignalStack {};
    work();
}};
```

## Uncaught C++ exceptions

On macOS the module defines `__cxa_throw` in the app's image: every throw
compiled into it records `backtrace()` into a per-thread buffer and calls
libc++abi's own (`dlsym(RTLD_NEXT)`). When `std::terminate` runs for an
exception whose object is that thread's last recorded throw, the report's
`crashingThread` is the throw site and `terminateFrames` is where terminate
aborted. A throw from inside another dynamic library goes straight to
libc++abi and is not recorded, so its report has the terminate stack in
`crashingThread` and no `terminateFrames`. Recording costs one `backtrace()`
per throw while crash reporting is started.

On Windows an uncaught exception reaches the unhandled-exception filter with
`_CxxThrowException` still on the stack, so the handler's walk already starts
at the throw: `terminateFrames` stays empty there.

## Fallback report

When the handler process is gone at crash time (the pipe write fails on
macOS; on Windows the handler has exited before the crash or before it ended
the app), the crashing thread writes `pending/<id>/report.json` itself, and
the next launch delivers it through `pendingReportFound` like any other.
`start()` lays out the id and every path beforehand; at crash time the report
is written from the shared block into a fixed buffer, through a temporary file
and a rename, with `mkdir`/`open`/`write`/`rename` (`CreateDirectoryW`,
`CreateFileW`, `MoveFileExW` on Windows) and no allocation, and
`crashed-last-run` is set. Miro cannot write it, since it builds its values on
the heap, so `FallbackReport.cpp` writes the fixed JSON shape itself.

It carries `capturedBy: "app"`, the reason, message, fault address, the
crashing thread's id and frames (module path, base and offset; no symbols or
module UUIDs, since nothing resolves them at crash time; the reporter's own
frames after the first are left in), `metadata`, `tags`, `extra` and `user`.
No breadcrumbs, attachments or minidump. On Windows the thread walks its own
stack with `RtlCaptureStackBackTrace` and names each module with
`RtlPcToFileHeader` and `GetModuleFileNameW`. Faults Windows reports only from
the exit code (heap corruption, `__fastfail`) are lost without a handler.

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
installs an alternate signal stack, starts recording throw sites, records every loaded image (and dyld)
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
- A stack overflow is reported only on a thread with an alternate stack (see
  Alternate signal stacks).
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

Both: when the handler process is gone at crash time, the app writes the
fallback report above.
