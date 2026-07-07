#pragma once

#if defined(__x86_64__) || defined(_M_X64)

#include <cstddef>
#include <cstdint>
#include <immintrin.h>

// AVX2 backend (256-bit). Only ever compiled inside Tu/Avx2.cpp, which carries
// the -mavx2/-mfma (or /arch:AVX2) flag and is reached only through the runtime
// dispatcher when the CPU supports AVX2 + FMA.
namespace eacp::simd::backend
{

struct Avx2
{
    // Eight unsigned 32-bit lanes (256-bit).
    struct U32
    {
        U32 operator&(U32 o) const { return {_mm256_and_si256(v, o.v)}; }
        U32 operator|(U32 o) const { return {_mm256_or_si256(v, o.v)}; }

        template <int N>
        U32 shl() const
        {
            return {_mm256_slli_epi32(v, N)};
        }

        template <int N>
        U32 shr() const
        {
            return {_mm256_srli_epi32(v, N)};
        }

        static U32 broadcast(std::uint32_t x)
        {
            return {_mm256_set1_epi32(static_cast<int>(x))};
        }

        static U32 load(const std::uint8_t* p)
        {
            return {_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))};
        }

        static void store(std::uint8_t* p, U32 a)
        {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), a.v);
        }

        __m256i v;
        static constexpr std::size_t lanes = 8;
    };
};

} // namespace eacp::simd::backend

#endif
