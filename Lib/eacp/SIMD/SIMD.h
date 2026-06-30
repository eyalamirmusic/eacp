#pragma once

#include <eacp/SIMD/Dispatch/Cpu.h>

#include <cstddef>
#include <cstdint>

// eacp-simd: a small, self-contained portable SIMD layer for image hot loops.
// Public kernels pick the fastest backend available on the host once, at first
// call, through a function pointer; the per-backend entry points are also
// exposed so tests and benchmarks can drive a specific backend directly.
namespace eacp::simd
{

// Swap the red and blue channels of `pixelCount` tightly-packed 8-bit RGBA
// pixels (RGBA <-> BGRA), writing to `out`. `in` and `out` may be equal
// (in place) but must not otherwise overlap.
void swapRedBlue(const std::uint8_t* in, std::uint8_t* out, std::size_t pixelCount);

// Resize a tightly-packed RGBA8 image with bilinear sampling, half-pixel-center
// mapping and edge clamping (OpenCV semantics). `dst` holds dstW*dstH*4 bytes.
// Caller guarantees src/dst dimensions are positive and buffers correctly sized.
void resizeBilinear(const std::uint8_t* src,
                    int srcWidth,
                    int srcHeight,
                    std::uint8_t* dst,
                    int dstWidth,
                    int dstHeight);

// Backend-specific entry points. Only the entries valid for the architecture
// the library was built for are declared and defined. EACP_SIMD_HAS_AVX2 is a
// PUBLIC compile definition set by the build when the AVX2 translation unit is
// compiled (single-arch x86-64 only), so consumers see a consistent view.
namespace backends
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
#endif

} // namespace backends
} // namespace eacp::simd
