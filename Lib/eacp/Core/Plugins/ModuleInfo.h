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

// An address inside this copy's image, which a library hands its executable
// for EACP_CLAIM_COPY. On Windows it is the image's HMODULE.
const void* getCurrentModuleAddress();

// A short hex token derived from this module's identity, stable for the
// module's lifetime and distinct between eacp copies in one process. Used to
// uniquify names in process-global OS namespaces (Win32 window classes).
std::string getModuleIdentitySuffix();

// True when this eacp copy is a guest in a process it does not own: it lives
// in a dynamic library the executable has not claimed (ClaimCopy.h). Decided
// from the loaded images, never from paths, so it holds from the first static
// initialiser on. Platform::isDLL/isStandalone are the friendly spellings.
bool isDynamicLibrary();

// The image that is the application: the executable when this copy is the
// application, the copy's own image when it is a guest.
FilePath getProductModulePath();

#ifdef _WIN32
// This module's HINSTANCE (as void* to keep windows.h out of the header):
// the value CreateWindowExW/RegisterClassExW should use so a plugin registers
// against itself, not the host executable.
void* getCurrentModuleHandle();

// The HINSTANCE of getProductModulePath()'s image, where the app icon lives.
void* getProductModuleHandle();

// root + this module's identity suffix. Win32 window-class names live in a
// process-global registry; a host and a plugin registering the same literal
// name would silently share one class — and dispatch every window into
// whichever image registered first. A per-image name makes each eacp copy's
// registration independent.
std::wstring getUniqueWindowClassName(const wchar_t* root);
#endif
} // namespace eacp::Plugins
