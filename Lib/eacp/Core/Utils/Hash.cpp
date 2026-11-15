#include "Hash.h"

namespace eacp::Hashing
{
std::uint64_t fnv1a64(std::string_view text)
{
    constexpr auto offsetBasis = std::uint64_t {14695981039346656037ull};
    constexpr auto prime = std::uint64_t {1099511628211ull};

    auto hash = offsetBasis;

    for (auto c: text)
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= prime;
    }

    return hash;
}

} // namespace eacp::Hashing
