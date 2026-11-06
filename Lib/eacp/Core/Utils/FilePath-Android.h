#pragma once

/** @file Internal: hands FilePath its Android data directory; not for direct use. */

#include <string>

namespace eacp
{
void setAndroidDataDirectory(std::string path);
} // namespace eacp
