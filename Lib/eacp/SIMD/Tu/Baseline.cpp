#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/Kernels/ResizeBilinear.h>
#include <eacp/SIMD/Kernels/SwapRedBlue.h>
#include <eacp/SIMD/Kernels/WarpAffine.h>

// The per-architecture baseline backend, reached without a special compile
// flag: SSE2 on x86-64 (guaranteed by the ABI), NEON on AArch64 (mandatory).
// Arch-guarded so a wrong-architecture slice of a multi-arch build is a no-op.

#if defined(__x86_64__) || defined(_M_X64)

#include <eacp/SIMD/Backend/Sse2.h>

namespace eacp::simd::backends
{

void swapRedBlue_sse2(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount)
{
    kernels::swapRedBlueImpl<backend::Sse2>(in, out, pixelCount);
}

void resizeBilinear_sse2(const std::uint8_t* src,
                         int srcWidth,
                         int srcHeight,
                         std::uint8_t* dst,
                         int dstWidth,
                         int dstHeight)
{
    kernels::resizeBilinearImpl<backend::Sse2>(
        src, srcWidth, srcHeight, dst, dstWidth, dstHeight);
}

void warpAffineInverse_sse2(const std::uint8_t* src,
                            int srcWidth,
                            int srcHeight,
                            const float* inverse2x3,
                            std::uint8_t* dst,
                            int dstWidth,
                            int dstHeight)
{
    kernels::warpAffineInverseImpl<backend::Sse2>(
        src, srcWidth, srcHeight, inverse2x3, dst, dstWidth, dstHeight);
}

} // namespace eacp::simd::backends

#elif defined(__aarch64__) || defined(_M_ARM64)

#include <eacp/SIMD/Backend/Neon.h>

namespace eacp::simd::backends
{

void swapRedBlue_neon(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount)
{
    kernels::swapRedBlueImpl<backend::Neon>(in, out, pixelCount);
}

void resizeBilinear_neon(const std::uint8_t* src,
                         int srcWidth,
                         int srcHeight,
                         std::uint8_t* dst,
                         int dstWidth,
                         int dstHeight)
{
    kernels::resizeBilinearImpl<backend::Neon>(
        src, srcWidth, srcHeight, dst, dstWidth, dstHeight);
}

void warpAffineInverse_neon(const std::uint8_t* src,
                            int srcWidth,
                            int srcHeight,
                            const float* inverse2x3,
                            std::uint8_t* dst,
                            int dstWidth,
                            int dstHeight)
{
    kernels::warpAffineInverseImpl<backend::Neon>(
        src, srcWidth, srcHeight, inverse2x3, dst, dstWidth, dstHeight);
}

} // namespace eacp::simd::backends

#endif
