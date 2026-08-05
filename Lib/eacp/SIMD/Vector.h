#pragma once

#include <cstddef>

// eacp-simd: the register-level float vector.
//
// This is the OTHER half of the module, and it is worth being explicit about
// why there are two.
//
// SIMD.h is array-at-a-time: `add(a, b, out, count)` walks whole buffers, and
// the module compiles its implementation with contraction and fast-math off so
// that every result is bit-identical on every build and architecture. That is
// exactly right for audio, where a buffer is thousands of samples and the
// determinism is worth more than the last few percent.
//
// It is exactly wrong for the inner loop of a convolution. A microkernel holds
// its accumulators in REGISTERS across the reduction and touches memory once
// per operand, not once per operation; expressing it through array primitives
// would stream every accumulator back to memory on every step, which is the
// whole cost being avoided. And a multiply-add that is forbidden from becoming
// an FMA runs at half rate on hardware where the fused form is one instruction.
//
// So: this header is the register-level type. It is header-only ON PURPOSE, so
// it compiles at the *consumer's* settings rather than the module's — a caller
// that wants FMA contraction turns it on for its own target (lib/tflite-cpu
// does) without disturbing the bit-exactness guarantee the array primitives and
// the image kernels make.
//
// The width is whatever the translation unit is being compiled for. Nothing
// here is runtime-dispatched: a kernel that wants several ISAs compiles itself
// once per ISA into separate translation units, the way Tu/Avx2.cpp already
// does, rather than paying an indirect call per vector.

#if defined(__AVX512F__)
    #include <immintrin.h>
    #define EACP_SIMD_VECTOR_AVX512 1
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define EACP_SIMD_VECTOR_AVX2 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #include <emmintrin.h>
    #define EACP_SIMD_VECTOR_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define EACP_SIMD_VECTOR_NEON 1
#else
    #define EACP_SIMD_VECTOR_SCALAR 1
#endif

namespace eacp::simd
{

/*
A vector of floats, one register wide.

Deliberately small: load, store, broadcast, arithmetic, min/max and a
horizontal sum. That is everything a convolution, a depthwise pass, a fused
activation and a fully-connected head need, and every one of them maps to a
single instruction on every backend below.

`loadBroadcast` is the one that looks redundant next to `broadcast` and is not.
An outer-product microkernel - the shape a convolution takes once its weights
are packed with the output channel innermost - spends one of these per input
element and nothing else on that operand, and every backend has a single
instruction that loads and splats in one go (`ld1r`, `vbroadcastss`). Written as
`broadcast(*p)` it is a scalar load into the wrong register file followed by a
cross-file move, which on some cores is a dozen cycles of latency in the middle
of the reduction.

Loads and stores are UNALIGNED. A tensor slice starts at whatever channel
offset the graph gave it, so alignment cannot be assumed; on every CPU this
targets an unaligned load of an aligned address costs nothing.
*/
struct F32
{
#if defined(EACP_SIMD_VECTOR_AVX512)
    static constexpr std::size_t lanes = 16;
    using Native = __m512;

    static constexpr std::size_t registers = 32;

    static F32 zero() { return {_mm512_setzero_ps()}; }
    static F32 broadcast(float x) { return {_mm512_set1_ps(x)}; }
    static F32 load(const float* p) { return {_mm512_loadu_ps(p)}; }
    static F32 loadBroadcast(const float* p) { return {_mm512_set1_ps(*p)}; }
    static void store(float* p, F32 a) { _mm512_storeu_ps(p, a.v); }

    F32 operator+(F32 o) const { return {_mm512_add_ps(v, o.v)}; }
    F32 operator-(F32 o) const { return {_mm512_sub_ps(v, o.v)}; }
    F32 operator*(F32 o) const { return {_mm512_mul_ps(v, o.v)}; }

    static F32 min(F32 a, F32 b) { return {_mm512_min_ps(a.v, b.v)}; }
    static F32 max(F32 a, F32 b) { return {_mm512_max_ps(a.v, b.v)}; }

    // this + a * b, as one instruction.
    F32 fma(F32 a, F32 b) const { return {_mm512_fmadd_ps(a.v, b.v, v)}; }

    float reduceAdd() const { return _mm512_reduce_add_ps(v); }

#elif defined(EACP_SIMD_VECTOR_AVX2)
    static constexpr std::size_t lanes = 8;
    using Native = __m256;

    static constexpr std::size_t registers = 16;

    static F32 zero() { return {_mm256_setzero_ps()}; }
    static F32 broadcast(float x) { return {_mm256_set1_ps(x)}; }
    static F32 load(const float* p) { return {_mm256_loadu_ps(p)}; }
    static F32 loadBroadcast(const float* p) { return {_mm256_broadcast_ss(p)}; }
    static void store(float* p, F32 a) { _mm256_storeu_ps(p, a.v); }

    F32 operator+(F32 o) const { return {_mm256_add_ps(v, o.v)}; }
    F32 operator-(F32 o) const { return {_mm256_sub_ps(v, o.v)}; }
    F32 operator*(F32 o) const { return {_mm256_mul_ps(v, o.v)}; }

    static F32 min(F32 a, F32 b) { return {_mm256_min_ps(a.v, b.v)}; }
    static F32 max(F32 a, F32 b) { return {_mm256_max_ps(a.v, b.v)}; }

    F32 fma(F32 a, F32 b) const
    {
    #if defined(__FMA__) || defined(_MSC_VER)
        return {_mm256_fmadd_ps(a.v, b.v, v)};
    #else
        // -mavx2 without -mfma: the compiler still contracts this where the
        // consumer allows it, which is the point of the header being inline.
        return {_mm256_add_ps(v, _mm256_mul_ps(a.v, b.v))};
    #endif
    }

    float reduceAdd() const
    {
        const auto low = _mm256_castps256_ps128(v);
        const auto high = _mm256_extractf128_ps(v, 1);
        auto sum = _mm_add_ps(low, high);
        sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
        sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 1));
        return _mm_cvtss_f32(sum);
    }

#elif defined(EACP_SIMD_VECTOR_SSE2)
    static constexpr std::size_t lanes = 4;
    using Native = __m128;

    static constexpr std::size_t registers = 16;

    static F32 zero() { return {_mm_setzero_ps()}; }
    static F32 broadcast(float x) { return {_mm_set1_ps(x)}; }
    static F32 load(const float* p) { return {_mm_loadu_ps(p)}; }
    static F32 loadBroadcast(const float* p) { return {_mm_load1_ps(p)}; }
    static void store(float* p, F32 a) { _mm_storeu_ps(p, a.v); }

    F32 operator+(F32 o) const { return {_mm_add_ps(v, o.v)}; }
    F32 operator-(F32 o) const { return {_mm_sub_ps(v, o.v)}; }
    F32 operator*(F32 o) const { return {_mm_mul_ps(v, o.v)}; }

    static F32 min(F32 a, F32 b) { return {_mm_min_ps(a.v, b.v)}; }
    static F32 max(F32 a, F32 b) { return {_mm_max_ps(a.v, b.v)}; }

    F32 fma(F32 a, F32 b) const { return {_mm_add_ps(v, _mm_mul_ps(a.v, b.v))}; }

    float reduceAdd() const
    {
        auto sum = _mm_add_ps(v, _mm_movehl_ps(v, v));
        sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 1));
        return _mm_cvtss_f32(sum);
    }

#elif defined(EACP_SIMD_VECTOR_NEON)
    static constexpr std::size_t lanes = 4;
    using Native = float32x4_t;

    static constexpr std::size_t registers = 32;

    static F32 zero() { return {vdupq_n_f32(0.f)}; }
    static F32 broadcast(float x) { return {vdupq_n_f32(x)}; }
    static F32 load(const float* p) { return {vld1q_f32(p)}; }
    static F32 loadBroadcast(const float* p) { return {vld1q_dup_f32(p)}; }
    static void store(float* p, F32 a) { vst1q_f32(p, a.v); }

    F32 operator+(F32 o) const { return {vaddq_f32(v, o.v)}; }
    F32 operator-(F32 o) const { return {vsubq_f32(v, o.v)}; }
    F32 operator*(F32 o) const { return {vmulq_f32(v, o.v)}; }

    static F32 min(F32 a, F32 b) { return {vminq_f32(a.v, b.v)}; }
    static F32 max(F32 a, F32 b) { return {vmaxq_f32(a.v, b.v)}; }

    F32 fma(F32 a, F32 b) const { return {vfmaq_f32(v, a.v, b.v)}; }

    float reduceAdd() const { return vaddvq_f32(v); }

#else
    // No SIMD for this target. Present so a kernel written against this header
    // still COMPILES and still gives the right answer everywhere; it is not
    // expected to be fast, and no shipping target reaches it.
    static constexpr std::size_t lanes = 4;
    static constexpr std::size_t registers = 16;
    using Native = float[lanes];

    static F32 zero() { return broadcast(0.f); }

    static F32 broadcast(float x)
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = x;
        return result;
    }

    static F32 load(const float* p)
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = p[i];
        return result;
    }

    static F32 loadBroadcast(const float* p) { return broadcast(*p); }

    static void store(float* p, F32 a)
    {
        for (std::size_t i = 0; i < lanes; ++i)
            p[i] = a.v[i];
    }

    F32 operator+(F32 o) const
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = v[i] + o.v[i];
        return result;
    }

    F32 operator-(F32 o) const
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = v[i] - o.v[i];
        return result;
    }

    F32 operator*(F32 o) const
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = v[i] * o.v[i];
        return result;
    }

    static F32 min(F32 a, F32 b)
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
        return result;
    }

    static F32 max(F32 a, F32 b)
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
        return result;
    }

    F32 fma(F32 a, F32 b) const
    {
        F32 result {};
        for (std::size_t i = 0; i < lanes; ++i)
            result.v[i] = v[i] + a.v[i] * b.v[i];
        return result;
    }

    float reduceAdd() const
    {
        auto sum = 0.f;
        for (std::size_t i = 0; i < lanes; ++i)
            sum += v[i];
        return sum;
    }
#endif

    F32& operator+=(F32 o) { return *this = *this + o; }
    F32& operator*=(F32 o) { return *this = *this * o; }

    Native v;
};

// How many floats one F32 holds, for the loop bounds and the tile constants
// that have to agree with it.
inline constexpr std::size_t floatLanes = F32::lanes;

// How many vector registers the target architecture has. A microkernel sizes
// its register tile from this: the whole point of holding accumulators across a
// reduction is lost the moment there are more of them than the register file
// holds, and a tile that spills is slower than the smaller tile that does not.
// 32 on NEON and AVX-512, 16 on SSE2 and AVX2.
inline constexpr std::size_t vectorRegisters = F32::registers;

/*
A partial load / store, for the ragged tail of a channel axis that is not a
multiple of the vector width.

Deliberately a scalar loop rather than a masked instruction: masked forms differ
per ISA, and this runs at most once per row of a tensor whose channel counts
(16, 24, 32, 64, 80, 96, 112, 128, 192, 240, 480, 672, 1152) are all multiples
of eight anyway. It exists for correctness on the shapes that are not, not for
speed on the ones that are.
*/
inline F32 loadPartial(const float* p, std::size_t count)
{
    alignas(64) float staging[F32::lanes] {};
    for (std::size_t i = 0; i < count && i < F32::lanes; ++i)
        staging[i] = p[i];
    return F32::load(staging);
}

inline void storePartial(float* p, F32 a, std::size_t count)
{
    alignas(64) float staging[F32::lanes];
    F32::store(staging, a);
    for (std::size_t i = 0; i < count && i < F32::lanes; ++i)
        p[i] = staging[i];
}

} // namespace eacp::simd
