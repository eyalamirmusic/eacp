#pragma once

#include <cstdint>
#include <cstring>

namespace eacp::UI
{
// FNV-1a over a cached thing's own content - never over a caller-supplied key.
// A summary, not a proof: every user must compare the content before treating a
// match as a match. Floats are hashed by their bit patterns.
class ContentHash
{
public:
    void mix(std::uint32_t bits)
    {
        for (auto byte = 0; byte < 4; ++byte)
        {
            hash ^= (bits >> (byte * 8)) & 0xffu;
            hash *= prime;
        }
    }

    void mix(float value)
    {
        // Folds negative zero, the one value whose bits disagree with ==.
        if (value == 0.f)
            value = 0.f;

        auto bits = std::uint32_t {};
        std::memcpy(&bits, &value, sizeof(bits));

        mix(bits);
    }

    void mix(bool value) { mix((std::uint32_t) value); }
    void mix(int value) { mix((std::uint32_t) value); }

    std::uint64_t get() const { return hash; }

private:
    static constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t hash = 14695981039346656037ull;
};
} // namespace eacp::UI
