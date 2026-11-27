#pragma once

#include "../Utils/WinInclude.h"

namespace eacp::Processes
{
// An unsigned eacp app quits when the process that started it exits (the
// watchdog in EventLoop-Windows.cpp), which a detached child must not do. The
// launcher puts a named event in the child's handle table, where it lasts
// exactly as long as the child, and the child's loop looks for it under its
// own process id. Call before the child's first thread runs.
void markAsDetachedChild(HANDLE suspendedChild, DWORD childProcessId);

bool isCurrentProcessADetachedChild();
} // namespace eacp::Processes
