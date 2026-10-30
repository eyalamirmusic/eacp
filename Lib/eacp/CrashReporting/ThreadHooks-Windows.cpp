#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <atomic>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto stackOverflowReserveBytes = ULONG {64 * 1024};

std::atomic<bool> shouldInstallOnNewThreads {false};

// The loader calls every module's TLS callbacks on each thread's start, before
// its start routine, whichever module or thread pool created it.
void NTAPI reserveStackOnThreadAttach(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_THREAD_ATTACH
        && shouldInstallOnNewThreads.load(std::memory_order_relaxed))
        installStackOverflowStackOnCurrentThread();
}
} // namespace

// The reserve can only grow, so a thread that has as much keeps what it has.
bool installStackOverflowStackOnCurrentThread()
{
    auto current = ULONG {0};
    ::SetThreadStackGuarantee(&current);

    if (current >= stackOverflowReserveBytes)
        return false;

    auto reserve = stackOverflowReserveBytes;
    return ::SetThreadStackGuarantee(&reserve) != 0;
}

void setStackOverflowStackOnNewThreads(bool shouldInstall)
{
    shouldInstallOnNewThreads.store(shouldInstall);
}
} // namespace eacp::CrashReporting::Detail

// A static library's object is linked only for its symbols, so the linker is
// told to keep both the TLS directory and this callback in the image.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:eacpReserveStackOnThreadAttach")
#pragma section(".CRT$XLE", long, read)

extern "C" extern const PIMAGE_TLS_CALLBACK eacpReserveStackOnThreadAttach;
extern "C" __declspec(allocate(".CRT$XLE"))
const PIMAGE_TLS_CALLBACK eacpReserveStackOnThreadAttach =
    &eacp::CrashReporting::Detail::reserveStackOnThreadAttach;
