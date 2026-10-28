# Crash matrix

Generated from `Zoo/Matrix.cpp` by `SelfDestruct --matrix <file>`; `CrashMatrix/fileIsCurrent` fails when this file differs. Every reported cell on the lane a test binary is built for is run by `Tests/CrashReporting`.

Legend: ✅ reported (who captured it, stack, minidump) · ⚠️ reported with a limit, listed under Gaps · ❌ not reported · ⏳ pending a PR · — not available on that lane.

## macOS arm64

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ SIGSEGV (handler, stack) | ✅ SIGSEGV (handler, stack) |
| Divide by zero | — | — |
| Stack overflow | ✅ SIGSEGV (handler, stack) | ⚠️ SIGBUS (handler, stack) |
| abort() | ✅ SIGABRT (handler, stack) | ✅ SIGABRT (handler, stack) |
| Pure virtual call | ✅ SIGABRT (handler, stack) | ✅ SIGABRT (handler, stack) |
| Uncaught exception | ✅ std::terminate (handler, stack) | ✅ std::terminate (handler, stack) |
| Heap corruption | ✅ SIGTRAP (handler, stack) | ⚠️ SIGABRT (handler, stack) |
| Hang | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| Spin | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| Deadlock | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| __fastfail | — | — |

## macOS x86_64

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ SIGSEGV (handler, stack) | ✅ SIGSEGV (handler, stack) |
| Divide by zero | ✅ SIGFPE (handler, stack) | ✅ SIGFPE (handler, stack) |
| Stack overflow | ✅ SIGSEGV (handler, stack) | ⚠️ SIGBUS (handler, stack) |
| abort() | ✅ SIGABRT (handler, stack) | ✅ SIGABRT (handler, stack) |
| Pure virtual call | ✅ SIGABRT (handler, stack) | ✅ SIGABRT (handler, stack) |
| Uncaught exception | ✅ std::terminate (handler, stack) | ✅ std::terminate (handler, stack) |
| Heap corruption | ✅ SIGTRAP (handler, stack) | ⚠️ SIGABRT (handler, stack) |
| Hang | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| Spin | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| Deadlock | ✅ Hang (handler, stack) | ⏳ PR not opened yet |
| __fastfail | — | — |

## Windows x64 MSVC

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) |
| Divide by zero | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) |
| Stack overflow | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) |
| abort() | ✅ SIGABRT (handler, stack, minidump) | ✅ SIGABRT (handler, stack, minidump) |
| Pure virtual call | ✅ purecall (handler, stack, minidump) | ✅ purecall (handler, stack, minidump) |
| Uncaught exception | ✅ std::terminate (handler, stack, minidump) | ✅ std::terminate (handler, stack, minidump) |
| Heap corruption | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) |
| Hang | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Spin | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Deadlock | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| __fastfail | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) |

## Windows x64 clang-cl

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) |
| Divide by zero | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) |
| Stack overflow | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) |
| abort() | ✅ SIGABRT (handler, stack, minidump) | ✅ SIGABRT (handler, stack, minidump) |
| Pure virtual call | ✅ purecall (handler, stack, minidump) | ✅ purecall (handler, stack, minidump) |
| Uncaught exception | ✅ std::terminate (handler, stack, minidump) | ⚠️ SIGABRT (handler, stack, minidump) |
| Heap corruption | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) |
| Hang | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Spin | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Deadlock | ✅ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| __fastfail | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) |

## Windows ARM64 MSVC

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) |
| Divide by zero | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) | ✅ EXCEPTION_INT_DIVIDE_BY_ZERO (handler, stack, minidump) |
| Stack overflow | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) |
| abort() | ✅ SIGABRT (handler, stack, minidump) | ⚠️ SIGABRT (handler, stack, minidump) |
| Pure virtual call | ✅ purecall (handler, stack, minidump) | ⚠️ purecall (handler, stack, minidump) |
| Uncaught exception | ⚠️ std::terminate (handler, stack, minidump) | ⚠️ std::terminate (handler, stack, minidump) |
| Heap corruption | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) |
| Hang | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Spin | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Deadlock | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| __fastfail | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) |

## Windows ARM64 clang-cl

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) | ✅ EXCEPTION_ACCESS_VIOLATION (handler, stack, minidump) |
| Divide by zero | — | — |
| Stack overflow | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) | ✅ EXCEPTION_STACK_OVERFLOW (handler, stack, minidump) |
| abort() | ✅ SIGABRT (handler, stack, minidump) | ⚠️ SIGABRT (handler, stack, minidump) |
| Pure virtual call | ✅ purecall (handler, stack, minidump) | ⚠️ purecall (handler, stack, minidump) |
| Uncaught exception | ⚠️ std::terminate (handler, stack, minidump) | ⚠️ SIGABRT (handler, stack, minidump) |
| Heap corruption | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) | ⚠️ STATUS_HEAP_CORRUPTION (exit-code, no stack) |
| Hang | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Spin | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| Deadlock | ⚠️ Hang (handler, stack, minidump) | ⏳ PR not opened yet |
| __fastfail | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) | ⚠️ STATUS_STACK_BUFFER_OVERRUN (exit-code, no stack) |

## Linux

| Fault | Main thread | Background thread |
|---|---|---|
| Null pointer | — | — |
| Divide by zero | — | — |
| Stack overflow | — | — |
| abort() | — | — |
| Pure virtual call | — | — |
| Uncaught exception | — | — |
| Heap corruption | — | — |
| Hang | — | — |
| Spin | — | — |
| Deadlock | — | — |
| __fastfail | — | — |

## Gaps

- macOS arm64: capture is in-process with backtrace(): no minidump, and no stacks of other threads (PR #182)
- macOS x86_64: capture is in-process with backtrace(): no minidump, and no stacks of other threads (PR #182)
- Windows x64 MSVC: with the handler gone, faults seen only in the exit code (heap corruption, __fastfail) are lost (PR #180)
- Windows x64 clang-cl: with the handler gone, faults seen only in the exit code (heap corruption, __fastfail) are lost (PR #180)
- Windows ARM64 MSVC: with the handler gone, faults seen only in the exit code (heap corruption, __fastfail) are lost (PR #180)
- Windows ARM64 clang-cl: with the handler gone, faults seen only in the exit code (heap corruption, __fastfail) are lost (PR #180)
- macOS x86_64: no machine runs this lane: its cells are expected, not verified
- Linux: no crash reporter on Linux
- ⚠️ macOS arm64, stackoverflow on the background thread: a secondary thread's guard page is a protection fault, so SIGBUS; every frame is the recursion
- ⚠️ macOS arm64, heapcorruption on the background thread: another thread's blocks go through the zone allocator, which aborts; some runs trap instead
- ⏳ macOS arm64, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ macOS arm64, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ macOS arm64, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ macOS x86_64, stackoverflow on the background thread: a secondary thread's guard page is a protection fault, so SIGBUS; every frame is the recursion
- ⚠️ macOS x86_64, heapcorruption on the background thread: another thread's blocks go through the zone allocator, which aborts; some runs trap instead
- ⏳ macOS x86_64, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ macOS x86_64, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ macOS x86_64, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows x64 MSVC, heapcorruption on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows x64 MSVC, heapcorruption on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⏳ Windows x64 MSVC, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ Windows x64 MSVC, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ Windows x64 MSVC, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows x64 MSVC, fastfail on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows x64 MSVC, fastfail on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⚠️ Windows x64 clang-cl, terminate on the background thread: clang-cl's std::thread terminates with no exception current: no type or what()
- ⚠️ Windows x64 clang-cl, heapcorruption on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows x64 clang-cl, heapcorruption on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⏳ Windows x64 clang-cl, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ Windows x64 clang-cl, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⏳ Windows x64 clang-cl, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows x64 clang-cl, fastfail on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows x64 clang-cl, fastfail on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⚠️ Windows ARM64 MSVC, abort on the background thread: StackWalk64 stops inside the CRT, short of the faulting code
- ⚠️ Windows ARM64 MSVC, purevirtual on the background thread: StackWalk64 stops inside the CRT, short of the faulting code
- ⚠️ Windows ARM64 MSVC, terminate on the main thread: StackWalk64 stops at RaiseException, one frame, on the CI runners; the minidump has the thread
- ⚠️ Windows ARM64 MSVC, terminate on the background thread: StackWalk64 stops at RaiseException, one frame, on the CI runners; the minidump has the thread
- ⚠️ Windows ARM64 MSVC, heapcorruption on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows ARM64 MSVC, heapcorruption on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⚠️ Windows ARM64 MSVC, hang on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 MSVC, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 MSVC, spin on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 MSVC, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 MSVC, deadlock on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 MSVC, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 MSVC, fastfail on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows ARM64 MSVC, fastfail on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⚠️ Windows ARM64 clang-cl, abort on the background thread: StackWalk64 stops inside the CRT, short of the faulting code
- ⚠️ Windows ARM64 clang-cl, purevirtual on the background thread: StackWalk64 stops inside the CRT, short of the faulting code
- ⚠️ Windows ARM64 clang-cl, terminate on the main thread: StackWalk64 stops at RaiseException, one frame, on the CI runners; the minidump has the thread
- ⚠️ Windows ARM64 clang-cl, terminate on the background thread: clang-cl's std::thread terminates with no exception current: no type or what()
- ⚠️ Windows ARM64 clang-cl, heapcorruption on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows ARM64 clang-cl, heapcorruption on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
- ⚠️ Windows ARM64 clang-cl, hang on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 clang-cl, hang on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 clang-cl, spin on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 clang-cl, spin on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 clang-cl, deadlock on the main thread: the frame-pointer walk can skip a stalled leaf built without a frame record
- ⏳ Windows ARM64 clang-cl, deadlock on the background thread: only the main thread is watched for hangs (PR not opened yet)
- ⚠️ Windows ARM64 clang-cl, fastfail on the main thread: from the exit code: no stack, no minidump (PR #180)
- ⚠️ Windows ARM64 clang-cl, fastfail on the background thread: from the exit code: no stack, no minidump, and the report names no crashing thread (PR #180)
