#pragma once

// Internal header: the per-backend entry points for the image kernels, used by
// the runtime dispatcher, the unit tests and the benchmark. This is NOT the
// public eacp-simd API -- include <eacp/SIMD/SIMD.h> for that. It is the one
// header where per-architecture / per-feature conditionals are allowed; the
// public interface stays free of them.

#include <eacp/SIMD/Dispatch/Cpu.h>

#include <cstddef>
#include <cstdint>

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
