#include <eacp/SIMD/Backend/Scalar.h>
#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/Kernels/ResizeBilinear.h>
#include <eacp/SIMD/Kernels/SwapRedBlue.h>
#include <eacp/SIMD/Kernels/WarpAffine.h>

// Scalar backend entry points. Always compiled, on every architecture: it is
// the correctness oracle and the SIMD kernels' tail fallback.
namespace eacp::simd::backends
{

void swapRedBlue_scalar(const std::uint8_t* in,
                        std::uint8_t* out,
                        std::size_t pixelCount)
{
    kernels::swapRedBlueImpl<backend::Scalar>(in, out, pixelCount);
}

void resizeBilinear_scalar(const std::uint8_t* src,
                           int srcWidth,
                           int srcHeight,
                           std::uint8_t* dst,
                           int dstWidth,
                           int dstHeight)
{
    kernels::resizeBilinearImpl<backend::Scalar>(
        src, srcWidth, srcHeight, dst, dstWidth, dstHeight);
}

void warpAffineInverse_scalar(const std::uint8_t* src,
                              int srcWidth,
                              int srcHeight,
                              const float* inverse2x3,
                              std::uint8_t* dst,
                              int dstWidth,
                              int dstHeight)
{
    kernels::warpAffineInverseImpl<backend::Scalar>(
        src, srcWidth, srcHeight, inverse2x3, dst, dstWidth, dstHeight);
}

} // namespace eacp::simd::backends
