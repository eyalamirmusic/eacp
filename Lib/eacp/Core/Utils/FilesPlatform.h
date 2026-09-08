#pragma once

#include <string>

namespace eacp::Detail
{

// The bundle's own resource lookup, implemented in Files.mm and empty on
// platforms with no bundle, so Files.cpp carries no platform switches.
std::string bundleResourcePath(const std::string& filename);

} // namespace eacp::Detail
