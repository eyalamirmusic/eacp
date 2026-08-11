#pragma once

#include "../Utils/Common.h"
#include "../Utils/FilePath.h"

namespace eacp::Plugins
{
// The module (executable, dylib or DLL) holding this eacp copy, resolved from
// the code's own address, never from the process. Every copy answers for itself.
FilePath getCurrentModulePath();

// Stable for the module's lifetime and distinct between eacp copies in one
// process, to uniquify names in process-global OS namespaces.
std::string getModuleIdentitySuffix();

// Platform::isDLL/isStandalone are the friendly spellings.
bool isDynamicLibrary();

#ifdef _WIN32
// This module's HINSTANCE, as void* to keep windows.h out of the header. Use it
// so a plugin registers and looks up against itself, not the host executable.
void* getCurrentModuleHandle();

// root + getModuleIdentitySuffix(). Win32 class names are process-global, so a
// host and a plugin registering one literal name would silently share a class.
std::wstring getUniqueWindowClassName(const wchar_t* root);
#endif
} // namespace eacp::Plugins
