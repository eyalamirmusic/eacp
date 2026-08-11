#pragma once

#include "../Common.h"

// Reference implementation (one lane): SIMD backends mirror it lane-for-lane,
// remainder tails run through it, and it is the oracle they are validated
// against.
namespace eacp::simd::backend
{

struct Scalar
{
    struct U32
    {
        U32 operator&(U32 o) const { return {v & o.v}; }
        U32 operator|(U32 o) const { return {v | o.v}; }

        template <int N>
        U32 shl() const
        {
            return {v << N};
        }

        template <int N>
        U32 shr() const
        {
            return {v >> N};
        }

        static U32 broadcast(std::uint32_t x) { return {x}; }

        static U32 load(const std::uint8_t* p)
        {
            std::uint32_t v;
            std::memcpy(&v, p, sizeof(v));
            return {v};
        }

        static void store(std::uint8_t* p, U32 a)
        {
            std::memcpy(p, &a.v, sizeof(a.v));
        }

        std::uint32_t v;
        static constexpr std::size_t lanes = 1;
    };

    // The four channels of one RGBA pixel.
    struct F4
    {
        F4 operator+(F4 o) const
        {
            return {{v[0] + o.v[0], v[1] + o.v[1], v[2] + o.v[2], v[3] + o.v[3]}};
        }

        F4 operator*(F4 o) const
        {
            return {{v[0] * o.v[0], v[1] * o.v[1], v[2] * o.v[2], v[3] * o.v[3]}};
        }

        static F4 broadcast(float x) { return {{x, x, x, x}}; }

        static F4 loadPixel(const std::uint8_t* p)
        {
            return {{static_cast<float>(p[0]),
                     static_cast<float>(p[1]),
                     static_cast<float>(p[2]),
                     static_cast<float>(p[3])}};
        }

        static void storePixel(std::uint8_t* out, F4 a)
        {
            for (int c = 0; c < 4; ++c)
            {
                const int r = static_cast<int>(std::lround(a.v[c]));
                out[c] = static_cast<std::uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            }
        }

        float v[4];
    };
};

} // namespace eacp::simd::backend
