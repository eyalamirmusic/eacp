#pragma once

#include <cstdint>
#include <cstring>

namespace eacp::UI
{
// FNV-1a over whatever a cached thing is made of, which is how everything in
// this tier that is worth keeping is keyed: a gradient by its stops, a path by
// its points. Never by anything the caller supplies -- a key somebody types is a
// key somebody gets wrong, and two different things sharing one entry is a
// failure that looks like corruption rather than like a mistake.
//
// A key is a summary and not a proof, so every user of one compares the content
// as well before it treats a match as a match. That turns a collision into a
// comparison instead of into a wrong picture.
//
// Floats go in as their bit patterns, which is what makes a thing rebuilt by the
// same arithmetic key the same way. The one place bits and values disagree about
// equality is zero, and that one is folded, so a stop list carrying a negative
// zero keys as the list it compares equal to rather than taking a second entry
// for colours that are already there.
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
