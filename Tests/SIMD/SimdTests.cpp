#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/SIMD.h>
#include <eacp/SIMD/Ops.h>

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

namespace
{
using WarpFn =
    void (*)(const std::uint8_t*, int, int, const float*, std::uint8_t*, int, int);

struct WarpCase
{
    int srcW, srcH, dstW, dstH;
    float m[6];
};

// Identity, scale+translate, rotate-ish, shear, a degenerate source width, and
// odd sizes -- exercising edge clamping and the affine coordinate math.
constexpr WarpCase kWarpCases[] = {
    {16, 16, 16, 16, {1.f, 0.f, 0.f, 0.f, 1.f, 0.f}},
    {16, 16, 20, 12, {0.7f, 0.f, 1.f, 0.f, 0.7f, 1.f}},
    {16, 16, 16, 16, {0.9f, -0.3f, 2.f, 0.3f, 0.9f, 1.f}},
    {16, 16, 24, 24, {0.5f, 0.2f, 0.f, 0.1f, 0.5f, 0.f}},
    {1, 16, 8, 8, {0.f, 0.f, 0.f, 0.f, 1.f, 0.f}},
    {33, 17, 9, 21, {1.3f, 0.1f, -2.f, -0.2f, 1.1f, 3.f}},
};

Pixels runWarp(WarpFn fn, const Pixels& src, const WarpCase& c)
{
    auto dst = Pixels(c.dstW * c.dstH * 4);
    fn(src.data(), c.srcW, c.srcH, c.m, dst.data(), c.dstW, c.dstH);
    return dst;
}

void checkWarpAgainstScalar(WarpFn fn)
{
    for (const auto& c: kWarpCases)
    {
        const auto src = makeImage(c.srcW, c.srcH);
        const auto got = runWarp(fn, src, c);
        const auto oracle =
            runWarp(&eacp::simd::backends::warpAffineInverse_scalar, src, c);
        check(got == oracle);
    }
}
} // namespace

auto tBaselineWarpMatchesScalar = test("SIMD/baselineWarpMatchesScalar") = []
{
#if defined(__x86_64__) || defined(_M_X64)
    checkWarpAgainstScalar(&eacp::simd::backends::warpAffineInverse_sse2);
#elif defined(__aarch64__) || defined(_M_ARM64)
    checkWarpAgainstScalar(&eacp::simd::backends::warpAffineInverse_neon);
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

namespace
{
// A length that is not a multiple of any backend's vector width, to exercise the
// scalar tail. Integer-valued data keeps every result exact (so the comparison
// is fusion-agnostic for multiplyAdd).
constexpr int kArrayCount = 1003;

EA::Vector<float> ramp(int stride, int offset)
{
    auto v = EA::Vector<float>(kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        v[i] = static_cast<float>((i % stride) + offset);
    return v;
}
} // namespace

auto tArrayAddMatchesReference = test("SIMD/arrayAddMatchesReference") = []
{
    const auto a = ramp(17, 1);
    const auto b = ramp(23, 0);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::add(a.data(), b.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] + b[i]);
};

auto tArraySubtractMatchesReference = test("SIMD/arraySubtractMatchesReference") = []
{
    const auto a = ramp(29, 5);
    const auto b = ramp(13, 0);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::subtract(a.data(), b.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] - b[i]);
};

auto tArrayMultiplyMatchesReference = test("SIMD/arrayMultiplyMatchesReference") = []
{
    const auto a = ramp(11, 0);
    const auto b = ramp(7, 1);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::multiply(a.data(), b.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] * b[i]);
};

auto tArrayMultiplyByScalarMatches =
    test("SIMD/arrayMultiplyByScalarMatchesReference") = []
{
    const auto a = ramp(19, 2);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::multiplyByScalar(a.data(), 3.f, out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] * 3.f);
};

auto tArrayMultiplyAddMatchesReference =
    test("SIMD/arrayMultiplyAddMatchesReference") = []
{
    const auto a = ramp(11, 0);
    const auto b = ramp(7, 1);
    const auto c = ramp(5, 0);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::multiplyAdd(a.data(), b.data(), c.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] * b[i] + c[i]);
};

auto tArrayMultiplyAddScalarMatchesReference =
    test("SIMD/arrayMultiplyAddScalarMatchesReference") = []
{
    const auto a = ramp(11, 0);
    const auto c = ramp(5, 0);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::multiplyAdd(a.data(), 3.f, c.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] * 3.f + c[i]);
};

// out == c is the accumulate-in-place shape the docs promise (out[i] += a[i]*b).
auto tArrayMultiplyAddScalarAccumulatesInPlace =
    test("SIMD/arrayMultiplyAddScalarAccumulatesInPlace") = []
{
    const auto a = ramp(11, 0);
    const auto before = ramp(5, 0);
    auto out = before;
    eacp::simd::multiplyAdd(a.data(), 2.f, out.data(), out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == before[i] + a[i] * 2.f);
};

// t = 0.25 keeps every intermediate exactly representable, so the comparison
// stays exact whether or not the compiler contracts the multiply-add.
auto tArrayLerpMatchesReference = test("SIMD/arrayLerpMatchesReference") = []
{
    const auto a = ramp(17, 1);
    const auto b = ramp(23, 0);
    auto out = EA::Vector<float>(kArrayCount);
    eacp::simd::lerp(a.data(), b.data(), 0.25f, out.data(), kArrayCount);
    for (int i = 0; i < kArrayCount; ++i)
        check(out[i] == a[i] + 0.25f * (b[i] - a[i]));
};

// Integer-valued data keeps the double accumulation exact regardless of the
// four-lane interleave, so the comparison against a sequential sum is exact.
auto tArraySumOfSquaresMatchesReference =
    test("SIMD/arraySumOfSquaresMatchesReference") = []
{
    const auto a = ramp(17, -8); // mixed signs
    auto expected = 0.0;
    for (int i = 0; i < kArrayCount; ++i)
        expected += (double) a[i] * (double) a[i];

    check(eacp::simd::sumOfSquares(a.data(), kArrayCount) == expected);
    check(eacp::simd::sumOfSquares(a.data(), 0) == 0.0);

    // Counts around the four-lane width exercise the tail loop.
    for (auto count: {1, 2, 3, 4, 5, 7, 8, 9})
    {
        auto partial = 0.0;
        for (int i = 0; i < count; ++i)
            partial += (double) a[i] * (double) a[i];
        check(eacp::simd::sumOfSquares(a.data(), (std::size_t) count) == partial);
    }
};

auto tArrayPeakAbsMatchesReference = test("SIMD/arrayPeakAbsMatchesReference") = []
{
    auto a = ramp(29, -14); // mixed signs; peak is a negative value's magnitude
    check(eacp::simd::peakAbs(a.data(), kArrayCount) == 14.f);
    check(eacp::simd::peakAbs(a.data(), 0) == 0.f);

    // The peak can land in any lane, including the tail.
    a[kArrayCount - 1] = -99.f;
    check(eacp::simd::peakAbs(a.data(), kArrayCount) == 99.f);
    a[2] = 200.f;
    check(eacp::simd::peakAbs(a.data(), kArrayCount) == 200.f);
};

// The buffer-level helpers in Ops.h: each forwards to the raw primitive, in
// place on its first argument.
auto tOpsHelpersForwardToPrimitives = test("SIMD/opsHelpersForwardToPrimitives") = []
{
    const auto a = ramp(17, 1);
    const auto b = ramp(23, 0);

    auto dst = a;
    eacp::simd::add(dst, b);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] + b[i]);

    dst = a;
    eacp::simd::subtract(dst, b);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] - b[i]);

    dst = a;
    eacp::simd::multiply(dst, b);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] * b[i]);

    dst = a;
    eacp::simd::multiply(dst, 3.f);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] * 3.f);

    dst = a;
    eacp::simd::multiplyAdd(dst, b, 2.f);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] + b[i] * 2.f);

    dst = a;
    eacp::simd::multiplyAdd(dst, a, b);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] + a[i] * b[i]);

    dst = a;
    eacp::simd::lerp(dst, b, 0.25f);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == a[i] + 0.25f * (b[i] - a[i]));

    check(eacp::simd::sumOfSquares(a)
          == eacp::simd::sumOfSquares(a.data(), kArrayCount));
    check(eacp::simd::peakAbs(a) == eacp::simd::peakAbs(a.data(), kArrayCount));
};

// Mismatched sizes process the common prefix and never touch the tail.
auto tOpsHelpersStopAtShortestBuffer =
    test("SIMD/opsHelpersStopAtShortestBuffer") = []
{
    const auto a = ramp(17, 1);
    auto shorter = EA::Vector<float>(kArrayCount / 2);
    for (int i = 0; i < shorter.size(); ++i)
        shorter[i] = 1.f;

    auto dst = a;
    eacp::simd::add(dst, shorter);
    for (int i = 0; i < kArrayCount; ++i)
        check(dst[i] == (i < kArrayCount / 2 ? a[i] + 1.f : a[i]));
};
