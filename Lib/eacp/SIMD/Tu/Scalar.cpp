#include <eacp/SIMD/Backend/Scalar.h>
#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/Kernels/ResizeBilinear.h>
#include <eacp/SIMD/Kernels/SwapRedBlue.h>

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

} // namespace eacp::simd::backends
