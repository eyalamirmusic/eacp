#pragma once

#include "../Utils/FilePath.h"

namespace eacp::Plugins::Detail
{
// The path dladdr reports for the image holding this eacp copy, made absolute
// against the working directory. Implemented in ModuleInfo-Posix.cpp.
FilePath findLoadedImagePath();
} // namespace eacp::Plugins::Detail
