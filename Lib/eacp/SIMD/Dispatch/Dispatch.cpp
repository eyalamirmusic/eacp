#include <eacp/SIMD/SIMD.h>

// Public kernel entry points. Each resolves the best available backend once
// (CPUID on x86-64, fixed on every other architecture) and calls it through a
// function pointer. The indirect call is a deliberate, non-inlinable boundary
// between baseline and AVX2 code.
namespace eacp::simd
{
namespace
{

using SwapFn = void (*)(const std::uint8_t*, std::uint8_t*, std::size_t);

SwapFn pickSwapRedBlue() noexcept
{
#if defined(__x86_64__) || defined(_M_X64)
#if defined(EACP_SIMD_HAS_AVX2)
    if (cpu::hasAvx2Fma())
        return &backends::swapRedBlue_avx2;
#endif
    return &backends::swapRedBlue_sse2;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return &backends::swapRedBlue_neon;
#else
    return &backends::swapRedBlue_scalar;
#endif
}

using ResizeFn = void (*)(const std::uint8_t*, int, int, std::uint8_t*, int, int);

ResizeFn pickResizeBilinear() noexcept
{
    // Bilinear blends one pixel's four channels per 128-bit step, so AVX2's
    // 256-bit width adds nothing yet; x86 uses SSE2 until a 2-pixel AVX2 path
    // lands.
#if defined(__x86_64__) || defined(_M_X64)
    return &backends::resizeBilinear_sse2;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return &backends::resizeBilinear_neon;
#else
    return &backends::resizeBilinear_scalar;
#endif
}

} // namespace

void swapRedBlue(const std::uint8_t* in, std::uint8_t* out, std::size_t pixelCount)
{
    static const SwapFn fn = pickSwapRedBlue();
    fn(in, out, pixelCount);
}

void resizeBilinear(const std::uint8_t* src,
                    int srcWidth,
                    int srcHeight,
                    std::uint8_t* dst,
                    int dstWidth,
                    int dstHeight)
{
    static const ResizeFn fn = pickResizeBilinear();
    fn(src, srcWidth, srcHeight, dst, dstWidth, dstHeight);
}

} // namespace eacp::simd
