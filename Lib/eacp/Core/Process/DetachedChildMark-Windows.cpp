#include "DetachedChildMark-Windows.h"

#include <string>

namespace eacp::Processes
{
namespace
{
std::wstring markNameFor(DWORD processId)
{
    return L"Local\\eacp-detached-child-" + std::to_wstring(processId);
}
} // namespace

void markAsDetachedChild(HANDLE suspendedChild, DWORD childProcessId)
{
    auto mark = CreateEventW(nullptr, TRUE, FALSE, markNameFor(childProcessId).c_str());

    if (mark == nullptr)
        return;

    auto markInChild = HANDLE {};
    DuplicateHandle(GetCurrentProcess(),
                    mark,
                    suspendedChild,
                    &markInChild,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS);
    CloseHandle(mark);
}

bool isCurrentProcessADetachedChild()
{
    auto name = markNameFor(GetCurrentProcessId());
    auto mark = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());

    if (mark == nullptr)
        return false;

    CloseHandle(mark);
    return true;
}
} // namespace eacp::Processes
