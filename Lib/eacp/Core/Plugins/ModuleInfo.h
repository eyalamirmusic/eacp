#pragma once

#include "../Utils/Common.h"
#include "../Utils/FilePath.h"

// Every statically linked eacp copy in a process answers three questions about
// itself, and they have different answers (Docs/Build.md, "Who owns the
// process"):
//
// - Copy identity: the image (executable, dylib, DLL, .so) this copy is
//   compiled into, for per-image OS registrations that must not collide
//   between copies. Always the copy's own image.
// - Role: is this copy the application, or a guest in a process someone else
//   owns? A copy in the executable is the application; a copy in a library is
//   a guest unless the executable claims it (Core/Plugins/ClaimCopy.h).
// - Product image: the image that is the application as the user and the OS
//   see it. The executable for a copy that is the application, the copy's own
//   library for a guest.
namespace eacp::Plugins
{
// Identity: the path of the image holding this eacp copy, resolved from the
// code's own address, never from the process. Inside a plugin it answers for
// the plugin's image; in the executable, for the executable.
FilePath getCurrentModulePath();

// Identity: an address inside the image holding this eacp copy. It is what a
// library hands its executable for a claim (EACP_CLAIM_COPY); any address in
// the image names it equally. On Windows it is the image's HMODULE.
const void* getCurrentModuleAddress();

// Identity: a short hex token derived from this copy's image, stable for the
// image's lifetime and distinct between eacp copies in one process. Used to
// uniquify names in process-global OS namespaces (Win32 window classes).
std::string getModuleIdentitySuffix();

// Role: true when this copy is a guest in a process it does not own — it lives
// in a dynamic library the executable has not claimed. False for a copy in
// the executable and for one in a library the executable claims. The name is
// historical: a claimed copy is in a library and answers false. Decided from
// the loaded images alone (addresses, never paths), so it is right from the
// first static initialiser on, whatever order the images initialise in.
// Platform::isDLL/isStandalone are the friendly spellings.
bool isDynamicLibrary();

// Product: the path of the image that is the application. The process
// executable when this copy is the application (in it, or claimed by it), the
// copy's own image when it is a guest. Files::applicationPath() is built on it.
FilePath getProductModulePath();

#ifdef _WIN32
// Identity: this copy's HINSTANCE (as void* to keep windows.h out of the
// header): the value CreateWindowExW/RegisterClassExW should use so each copy
// registers against itself, never against another copy's image.
void* getCurrentModuleHandle();

// Product: the HINSTANCE of getProductModulePath()'s image, which is where the
// application's resources (its icon) are compiled.
void* getProductModuleHandle();

// root + this module's identity suffix. Win32 window-class names live in a
// process-global registry; a host and a plugin registering the same literal
// name would silently share one class — and dispatch every window into
// whichever image registered first. A per-image name makes each eacp copy's
// registration independent.
std::wstring getUniqueWindowClassName(const wchar_t* root);
#endif
} // namespace eacp::Plugins
