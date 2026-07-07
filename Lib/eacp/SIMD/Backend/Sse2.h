#pragma once

#if defined(__x86_64__) || defined(_M_X64)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

// SSE2 backend. SSE2 is part of the x86-64 baseline, so this header needs no
// special compile flag and lives in the ordinary (un-flagged) translation unit.
namespace eacp::simd::backend
{

struct Sse2
{
    // Four unsigned 32-bit lanes (128-bit).
    struct U32
    {
        U32 operator&(U32 o) const { return {_mm_and_si128(v, o.v)}; }
        U32 operator|(U32 o) const { return {_mm_or_si128(v, o.v)}; }

        template <int N>
        U32 shl() const
        {
            return {_mm_slli_epi32(v, N)};
        }

        template <int N>
        U32 shr() const
        {
            return {_mm_srli_epi32(v, N)};
        }

        static U32 broadcast(std::uint32_t x)
        {
            return {_mm_set1_epi32(static_cast<int>(x))};
        }

        static U32 load(const std::uint8_t* p)
        {
            return {_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))};
        }

        static void store(std::uint8_t* p, U32 a)
        {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a.v);
        }

        __m128i v;
        static constexpr std::size_t lanes = 4;
    };

    // The four channels of one RGBA pixel held as floats (128-bit).
    struct F4
    {
        F4 operator+(F4 o) const { return {_mm_add_ps(v, o.v)}; }
        F4 operator*(F4 o) const { return {_mm_mul_ps(v, o.v)}; }

        static F4 broadcast(float x) { return {_mm_set1_ps(x)}; }

        // Load four packed bytes (one RGBA pixel) and widen to four floats.
        static F4 loadPixel(const std::uint8_t* p)
        {
            int packed;
            std::memcpy(&packed, p, 4);
            const auto bytes = _mm_cvtsi32_si128(packed);
            const auto zero = _mm_setzero_si128();
            const auto words = _mm_unpacklo_epi8(bytes, zero);
            const auto dwords = _mm_unpacklo_epi16(words, zero);
            return {_mm_cvtepi32_ps(dwords)};
        }

        // Round each (non-negative) lane with floor(v + 0.5) -- equal to lround
        // here -- saturate to [0, 255], and store the four bytes.
        static void storePixel(std::uint8_t* out, F4 a)
        {
            const auto rounded =
                _mm_cvttps_epi32(_mm_add_ps(a.v, _mm_set1_ps(0.5f)));
            const auto words = _mm_packs_epi32(rounded, rounded);
            const auto bytes = _mm_packus_epi16(words, words);
            const int packed = _mm_cvtsi128_si32(bytes);
            std::memcpy(out, &packed, 4);
        }

        __m128 v;
    };
};

} // namespace eacp::simd::backend

#endif
