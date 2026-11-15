#pragma once

// The handler Process::requestStop() reaches in a child, implemented once per
// platform (StopRequest-Posix.cpp, StopRequest-Windows.cpp): SIGTERM on POSIX,
// CTRL_BREAK_EVENT through a console control handler on Windows. Only stores a
// flag, so the child mode that installs it polls.
namespace StopRequest
{
void installHandler();
bool wasRequested();
} // namespace StopRequest
