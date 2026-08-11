#pragma once

// Include only from *-Windows translation units and Windows-only headers; it
// carries no platform #if of its own.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// WIN32_LEAN_AND_MEAN omits IUnknown, which C++/WinRT needs defined before its
// own headers or the translation unit locks into a no-classic-COM mode.
#include <unknwn.h>
