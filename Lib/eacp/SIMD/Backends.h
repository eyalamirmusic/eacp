#pragma once

#include "Common.h"

// Internal per-backend entry points, and the one header where
// per-architecture / per-feature conditionals are allowed. Not the public API:
// include <eacp/SIMD/SIMD.h> for that.

#include "Dispatch/Cpu.h"

namespace eacp::simd::backends
{

void swapRedBlue_scalar(const std::uint8_t* in,
                        std::uint8_t* out,
                        std::size_t pixelCount);

void resizeBilinear_scalar(const std::uint8_t* src,
                           int srcWidth,
                           int srcHeight,
                           std::uint8_t* dst,
                           int dstWidth,
                           int dstHeight);

void warpAffineInverse_scalar(const std::uint8_t* src,
                              int srcWidth,
                              int srcHeight,
                              const float* inverse2x3,
                              std::uint8_t* dst,
                              int dstWidth,
                              int dstHeight);

#if defined(__x86_64__) || defined(_M_X64)
void swapRedBlue_sse2(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount);
void resizeBilinear_sse2(const std::uint8_t* src,
                         int srcWidth,
                         int srcHeight,
                         std::uint8_t* dst,
                         int dstWidth,
                         int dstHeight);
void warpAffineInverse_sse2(const std::uint8_t* src,
                            int srcWidth,
                            int srcHeight,
                            const float* inverse2x3,
                            std::uint8_t* dst,
                            int dstWidth,
                            int dstHeight);
#if defined(EACP_SIMD_HAS_AVX2)
void swapRedBlue_avx2(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount);
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
void swapRedBlue_neon(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount);
void resizeBilinear_neon(const std::uint8_t* src,
                         int srcWidth,
                         int srcHeight,
                         std::uint8_t* dst,
                         int dstWidth,
                         int dstHeight);
void warpAffineInverse_neon(const std::uint8_t* src,
                            int srcWidth,
                            int srcHeight,
                            const float* inverse2x3,
                            std::uint8_t* dst,
                            int dstWidth,
                            int dstHeight);
#endif

} // namespace eacp::simd::backends
