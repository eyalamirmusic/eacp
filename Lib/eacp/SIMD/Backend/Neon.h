#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)

#include <arm_neon.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// NEON backend (128-bit). NEON is mandatory on AArch64, so it is the
// unconditional baseline there and needs no runtime dispatch or compile flag.
namespace eacp::simd::backend
{

struct Neon
{
    // Four unsigned 32-bit lanes (128-bit).
    struct U32
    {
        U32 operator&(U32 o) const { return {vandq_u32(v, o.v)}; }
        U32 operator|(U32 o) const { return {vorrq_u32(v, o.v)}; }

        template <int N>
        U32 shl() const
        {
            return {vshlq_n_u32(v, N)};
        }

        template <int N>
        U32 shr() const
        {
            return {vshrq_n_u32(v, N)};
        }

        static U32 broadcast(std::uint32_t x) { return {vdupq_n_u32(x)}; }

        static U32 load(const std::uint8_t* p)
        {
            return {vld1q_u32(reinterpret_cast<const std::uint32_t*>(p))};
        }

        static void store(std::uint8_t* p, U32 a)
        {
            vst1q_u32(reinterpret_cast<std::uint32_t*>(p), a.v);
        }

        uint32x4_t v;
        static constexpr std::size_t lanes = 4;
    };

    // The four channels of one RGBA pixel held as floats (128-bit).
    struct F4
    {
        F4 operator+(F4 o) const { return {vaddq_f32(v, o.v)}; }
        F4 operator*(F4 o) const { return {vmulq_f32(v, o.v)}; }

        static F4 broadcast(float x) { return {vdupq_n_f32(x)}; }

        // Load four packed bytes (one RGBA pixel) and widen to four floats.
        static F4 loadPixel(const std::uint8_t* p)
        {
            std::uint32_t packed;
            std::memcpy(&packed, p, 4);
            const auto bytes = vreinterpret_u8_u32(vdup_n_u32(packed));
            const auto words = vget_low_u16(vmovl_u8(bytes));
            return {vcvtq_f32_u32(vmovl_u16(words))};
        }

        // Round each (non-negative) lane with floor(v + 0.5) -- equal to lround
        // here -- saturate to [0, 255], and store the four bytes.
        static void storePixel(std::uint8_t* out, F4 a)
        {
            const auto rounded = vcvtq_s32_f32(vaddq_f32(a.v, vdupq_n_f32(0.5f)));
            const auto words = vqmovun_s32(rounded);
            const auto bytes = vqmovn_u16(vcombine_u16(words, words));
            const auto packed = vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
            std::memcpy(out, &packed, 4);
        }

        float32x4_t v;
    };
};

} // namespace eacp::simd::backend

#endif
