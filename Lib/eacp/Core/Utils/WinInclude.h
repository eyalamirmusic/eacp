#pragma once

// Windows-only convenience header — included exclusively from *-Windows
// translation units (and Windows-only headers like D3DTypes.h), so it is no
// longer guarded by a platform #if. The macro guards below are standard
// windows.h configuration, not platform branches.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// windows.h with WIN32_LEAN_AND_MEAN omits the COM headers, so IUnknown is not
// defined. C++/WinRT needs <unknwn.h> included before its headers to enable
// classic COM interop (e.g. ICompositorDesktopInterop); without it the first
// winrt/base.h include locks the translation unit into a no-classic-COM mode,
// which breaks unity builds that mix WinRT interop across files.
#include <unknwn.h>

namespace eacp
{
// GetProcAddress answers in FARPROC, a signature that matches nothing it ever
// resolves, so casting it straight to the real one is what clang flags as
// -Wcast-function-type-mismatch. Going through void* is the sanctioned
// spelling for "this really is that function".
template <typename Fn>
Fn procAddress(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(module, name)));
}
} // namespace eacp
