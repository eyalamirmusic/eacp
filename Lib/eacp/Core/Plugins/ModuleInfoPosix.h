#pragma once

#include "../Utils/FilePath.h"

namespace eacp::Plugins::Detail
{
// The path dladdr reports for the image holding this eacp copy, made absolute
// against the working directory. Right for a library, whose path the loader
// recorded; for the main program glibc reports argv[0] instead, so Linux asks
// the kernel there. Implemented in ModuleInfo-Posix.cpp.
FilePath loadedImagePath();
} // namespace eacp::Plugins::Detail
