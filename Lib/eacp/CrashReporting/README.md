# CrashReporting

When an app faults, a second process writes a report with the crashed
thread's stack, the app's metadata and its attachments, ends the app, and
hands the report to the app's own code. macOS and Windows.

## Use

```cpp
struct MyCrashHandler : CrashReporting::CrashHandler
{
    // In the handler process, after the app is gone. May open windows.
    void crashReported(const CrashReporting::CrashReport& report) override;

    // In the app, from start(), for each report still in pending/.
    void pendingReportFound(const CrashReporting::CrashReport& report) override;
};

EACP_REGISTER_CRASH_HANDLER(MyCrashHandler);

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    CrashReporting::start();
    CrashReporting::setMetadata("build", "1234");
    CrashReporting::addAttachment(logPath);
    ...
}
```

CMake: `eacp_enable_crash_reporting(<executable>)` links `eacp-crash-reporting`
and defines `EACP_CRASH_REPORTING=1`. It refuses anything but an executable,
since the handler is that executable relaunched as
`<exe> --eacp-crash-handler <database> <channel> -- <Options::handlerArguments>`.

`Options`: `databaseDirectory` (default `appSupportDirectory()/Crashes`),
`maxHandledReports`, `handlerStartTimeout`, `channelName` (Windows object
names; default `createDefaultChannelName()`), `handlerArguments` (what
`Apps::commandLineArgs()` holds in the handler). `isHandlerRunning()` says
whether a crash right now would be caught. `markReportHandled(id)` moves a
report from `pending/` to `handled/` and prunes the latter.

## Report

```
<database>/pending/<id>/report.json
                       minidump.dmp       Windows only
                       attachments/<copies>
<database>/handled/<id>/...
```

`report.json` is written last, so a folder without one is a capture that did
not finish; it is read back as `reason: "incomplete"`. Fields: `id` (UUID),
`timestamp` (ms since epoch), `reason`, `message`, `faultAddress`,
`crashingThread {threadId, frames[{modulePath, moduleBase, offset, symbol}]}`,
`minidumpPath`, `metadata`, `attachments`, `platform`, `capturedBy`
(`handler`, `exit-code`, `incomplete`). Addresses are hex strings, since a
JSON number loses the low bits of a 64-bit address.

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

Not caught: `SIGKILL`, the OOM killer, a hang, a crash while the handler is
not running (it is re-raised and lost), and on Windows anything that bypasses
the unhandled-exception filter other than through the exit code above.

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

Both: when the handler process is gone at crash time, the crash is not
reported; there is no in-process fallback in v1.
