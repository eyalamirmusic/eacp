#pragma once

#include "../Utils/FilePath.h"

namespace eacp::Plugins::Detail
{
// Platform-provided primitives behind DynamicLibrary, implemented in
// DynamicLibrary-Posix.cpp and DynamicLibrary-Windows.cpp so the refcounted
// registry in DynamicLibrary.cpp carries no platform switches.

// dlopen/LoadLibrary with local, eager binding. Returns nullptr on failure.
void* loadImage(const FilePath& path);

// dlclose/FreeLibrary. The registry calls loadImage once per path and
// unloadImage once at the last close, so the image genuinely unmaps here.
void unloadImage(void* handle);

// dlsym/GetProcAddress. `handle` must be a live loadImage result.
void* findImageSymbol(void* handle, const std::string& name);
} // namespace eacp::Plugins::Detail
