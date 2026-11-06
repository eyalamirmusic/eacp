#pragma once

#include <cstdint>
#include <string_view>

namespace eacp::Hashing
{
// The standard 64-bit FNV-1a: stable across platforms, builds and runs, so it
// can name something a build wrote and a later run looks up. A summary, not a
// cryptographic digest.
std::uint64_t fnv1a64(std::string_view text);
} // namespace eacp::Hashing
