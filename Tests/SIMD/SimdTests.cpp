#include <eacp/SIMD/SIMD.h>

#include <NanoTest/NanoTest.h>
#include <ea_data_structures/ea_data_structures.h>

#include <cstddef>
#include <cstdint>

using namespace nano;

namespace
{
using Pixels = EA::Vector<std::uint8_t>;
using SwapFn = void (*)(const std::uint8_t*, std::uint8_t*, std::size_t);
using ResizeFn = void (*)(const std::uint8_t*, int, int, std::uint8_t*, int, int);

// Pixel counts chosen to straddle every backend's lane width (SSE2/NEON = 4,
// AVX2 = 8): zero, sub-lane, exact multiples, and odd remainders.
constexpr std::size_t kSwapSizes[] = {
    0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 64, 1000};

Pixels makePixels(std::size_t pixelCount)
{
    auto data = Pixels(static_cast<int>(pixelCount) * 4);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xFF);
    return data;
}

// Independent reference: swap byte 0 and byte 2 of every 4-byte pixel.
Pixels swapReference(const Pixels& in)
{
    auto out = in;
    const auto pixelCount = out.size() / 4;
    for (int p = 0; p < pixelCount; ++p)
    {
        const auto red = out[p * 4 + 0];
        out[p * 4 + 0] = out[p * 4 + 2];
        out[p * 4 + 2] = red;
    }
    return out;
}

void checkSwapAgainstScalar(SwapFn fn)
{
    for (auto count: kSwapSizes)
    {
        const auto in = makePixels(count);
        auto got = Pixels(in.size());
        auto oracle = Pixels(in.size());
        fn(in.data(), got.data(), count);
        eacp::simd::backends::swapRedBlue_scalar(in.data(), oracle.data(), count);
        check(got == oracle);
    }
}

struct ResizeCase
{
    int srcW, srcH, dstW, dstH;
};

// Up, down, identity, extreme aspect, and prime sizes to stress edge clamping
// and the per-pixel coordinate math in both directions.
constexpr ResizeCase kResizeCases[] = {
    {1, 1, 4, 4},
    {4, 4, 1, 1},
    {2, 2, 1, 1},
    {8, 8, 8, 8},
    {16, 9, 7, 13},
    {7, 13, 16, 9},
    {1, 10, 5, 3},
    {10, 1, 3, 5},
    {17, 17, 5, 5},
    {5, 5, 17, 17},
    {64, 48, 33, 21},
    {3, 3, 9, 9},
};

Pixels makeImage(int w, int h)
{
    auto data = Pixels(w * h * 4);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::uint8_t>((i * 53 + (i / 4) * 7 + 19) & 0xFF);
    return data;
}

Pixels runResize(ResizeFn fn, const Pixels& src, const ResizeCase& c)
{
    auto dst = Pixels(c.dstW * c.dstH * 4);
    fn(src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH);
    return dst;
}

void checkResizeAgainstScalar(ResizeFn fn)
{
    for (const auto& c: kResizeCases)
    {
        const auto src = makeImage(c.srcW, c.srcH);
        const auto got = runResize(fn, src, c);
        const auto oracle =
            runResize(&eacp::simd::backends::resizeBilinear_scalar, src, c);
        check(got == oracle);
    }
}
} // namespace

auto tScalarSwapMatchesReference = test("SIMD/scalarSwapMatchesReference") = []
{
    for (auto count: kSwapSizes)
    {
        const auto in = makePixels(count);
        auto got = Pixels(in.size());
        eacp::simd::backends::swapRedBlue_scalar(in.data(), got.data(), count);
        check(got == swapReference(in));
    }
};

auto tBaselineSwapMatchesScalar = test("SIMD/baselineSwapMatchesScalar") = []
{
#if defined(__x86_64__) || defined(_M_X64)
    checkSwapAgainstScalar(&eacp::simd::backends::swapRedBlue_sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
    checkSwapAgainstScalar(&eacp::simd::backends::swapRedBlue_neon);
#endif
};

auto tAvx2SwapMatchesScalar = test("SIMD/avx2SwapMatchesScalar") = []
{
#if defined(EACP_SIMD_HAS_AVX2)
    if (eacp::simd::cpu::hasAvx2Fma())
        checkSwapAgainstScalar(&eacp::simd::backends::swapRedBlue_avx2);
#endif
};

auto tSwapDispatchMatchesScalar = test("SIMD/swapDispatchMatchesScalar") = []
{
    const auto in = makePixels(257);
    auto got = Pixels(in.size());
    auto oracle = Pixels(in.size());
    eacp::simd::swapRedBlue(in.data(), got.data(), 257);
    eacp::simd::backends::swapRedBlue_scalar(in.data(), oracle.data(), 257);
    check(got == oracle);
};

auto tSwapTwiceRestoresOriginal = test("SIMD/swapTwiceRestoresOriginal") = []
{
    auto buffer = makePixels(123);
    const auto original = buffer;
    eacp::simd::swapRedBlue(buffer.data(), buffer.data(), 123);
    eacp::simd::swapRedBlue(buffer.data(), buffer.data(), 123);
    check(buffer == original);
};

auto tBaselineResizeMatchesScalar = test("SIMD/baselineResizeMatchesScalar") = []
{
#if defined(__x86_64__) || defined(_M_X64)
    checkResizeAgainstScalar(&eacp::simd::backends::resizeBilinear_sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
    checkResizeAgainstScalar(&eacp::simd::backends::resizeBilinear_neon);
#endif
};

auto tResizeIdentityReturnsSource = test("SIMD/resizeIdentityReturnsSource") = []
{
    // Same-size bilinear with half-pixel centers samples each pixel exactly, so
    // every backend must return the source untouched.
    const ResizeCase cases[] = {{5, 4, 5, 4}, {16, 9, 16, 9}};
    for (const auto& c: cases)
    {
        const auto src = makeImage(c.srcW, c.srcH);
        auto dst = Pixels(src.size());
        eacp::simd::resizeBilinear(
            src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH);
        check(dst == src);
    }
};
