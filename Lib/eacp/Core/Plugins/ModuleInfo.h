#pragma once

#include "../Utils/Common.h"
#include "../Utils/FilePath.h"

namespace eacp::Plugins
{
// Identity of the module (executable, dylib or DLL) that contains this eacp
// copy — resolved from the code's own address, never from the process. Inside
// a dlopen-loaded plugin these answer for the plugin's image; in the host
// they answer for the executable. Every eacp copy in a process gets its own
// answer, which is exactly what per-image resource lookup and per-image OS
// registrations need.
FilePath getCurrentModulePath();

// A short hex token derived from this module's identity, stable for the
// module's lifetime and distinct between eacp copies in one process. Used to
// uniquify names in process-global OS namespaces (Win32 window classes).
std::string getModuleIdentitySuffix();

// True when this eacp copy is compiled into a dynamic library (dylib, DLL,
// .so) rather than the process executable. Each copy answers for its own
// image, which is what run<T>()'s loop-ownership decision needs.
// Platform::isDLL/isStandalone are the friendly spellings.
bool isDynamicLibrary();

#ifdef _WIN32
// This module's HINSTANCE (as void* to keep windows.h out of the header):
// the value CreateWindowExW/RegisterClassExW/FindResourceW should use so a
// plugin registers and looks up against itself, not the host executable.
void* getCurrentModuleHandle();

// root + this module's identity suffix. Win32 window-class names live in a
// process-global registry; a host and a plugin registering the same literal
// name would silently share one class — and dispatch every window into
// whichever image registered first. A per-image name makes each eacp copy's
// registration independent.
std::wstring getUniqueWindowClassName(const wchar_t* root);
#endif
} // namespace eacp::Plugins
