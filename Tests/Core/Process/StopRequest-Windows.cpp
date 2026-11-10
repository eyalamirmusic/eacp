#include "StopRequest.h"

#include <atomic>

#include <eacp/Core/Utils/WinInclude.h>

namespace StopRequest
{
namespace
{
std::atomic<bool> requested {false};

BOOL WINAPI recordRequest(DWORD event)
{
    if (event != CTRL_BREAK_EVENT)
        return FALSE;

    requested.store(true);
    return TRUE;
}
} // namespace

void installHandler()
{
    SetConsoleCtrlHandler(recordRequest, TRUE);
}

bool wasRequested()
{
    return requested.load();
}
} // namespace StopRequest
