#pragma once

#include "../Utils/FilePath.h"

namespace eacp::Plugins::Detail
{
// dlopen/LoadLibrary with local, eager binding. Returns nullptr on failure.
void* loadImage(const FilePath& path);

// dlclose/FreeLibrary. Called once at the last close, so the image really
// unmaps here.
void unloadImage(void* handle);

// dlsym/GetProcAddress. `handle` must be a live loadImage result.
void* findImageSymbol(void* handle, const std::string& name);
} // namespace eacp::Plugins::Detail
