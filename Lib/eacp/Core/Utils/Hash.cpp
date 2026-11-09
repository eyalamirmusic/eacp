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

std::string toHex(std::uint64_t value)
{
    constexpr auto digits = std::string_view {"0123456789abcdef"};
    constexpr auto length = 16;

    auto text = std::string(length, '0');

    for (auto index = length - 1; index >= 0; --index)
    {
        text[(std::size_t) index] = digits[value & 0xfu];
        value >>= 4;
    }

    return text;
}
} // namespace eacp::Hashing
