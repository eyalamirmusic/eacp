#include <eacp/SIMD/SIMD.h>

#include <ea_data_structures/ea_data_structures.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Micro-benchmark comparing the scalar reference against the active SIMD backend
// for the eacp-simd kernels. Build Release (or RelWithDebInfo) -- a Debug build
// compiles every backend at -O0, which makes the comparison meaningless.

namespace
{
using Pixels = EA::Vector<std::uint8_t>;
using ResizeFn = void (*)(const std::uint8_t*, int, int, std::uint8_t*, int, int);
using SwapFn = void (*)(const std::uint8_t*, std::uint8_t*, std::size_t);

// A volatile sink that consumes one output byte per iteration so the optimizer
// cannot elide the calls. Combined with perturbing an input byte per iteration
// (below) it also prevents hoisting the loop-invariant call out of the loop.
volatile std::uint64_t gSink = 0;

Pixels makeBuffer(int byteCount)
{
    auto data = Pixels(byteCount);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::uint8_t>((i * 53 + (i / 4) * 7 + 19) & 0xFF);
    return data;
}

// Returns milliseconds per iteration. `src` is perturbed each iteration so the
// call is not loop-invariant; one byte of the output is observed so the stores
// are not dead. Both are O(1) and negligible against a whole-image kernel.
template <class Run>
double timeMs(int iters, Pixels& src, const std::uint8_t* observe, Run&& run)
{
    run(); // warm up caches and the dispatch pointer

    std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
    {
        src.data()[i % src.size()] ^= static_cast<std::uint8_t>(i + 1);
        run();
        sink += observe[0];
    }
    const auto end = std::chrono::steady_clock::now();

    gSink ^= sink;
    return std::chrono::duration<double, std::milli>(end - start).count()
           / static_cast<double>(iters);
}

double megaPixelsPerSec(long long pixels, double ms)
{
    return static_cast<double>(pixels) / (ms * 1000.0);
}

const char* baselineName()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "sse2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "neon";
#else
    return "scalar";
#endif
}

ResizeFn baselineResize()
{
#if defined(__x86_64__) || defined(_M_X64)
    return &eacp::simd::backends::resizeBilinear_sse2;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return &eacp::simd::backends::resizeBilinear_neon;
#else
    return &eacp::simd::backends::resizeBilinear_scalar;
#endif
}

SwapFn baselineSwap()
{
#if defined(__x86_64__) || defined(_M_X64)
    return &eacp::simd::backends::swapRedBlue_sse2;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return &eacp::simd::backends::swapRedBlue_neon;
#else
    return &eacp::simd::backends::swapRedBlue_scalar;
#endif
}

struct ResizeConfig
{
    int srcW, srcH, dstW, dstH;
};

void benchResize(const ResizeConfig& c)
{
    auto src = makeBuffer(c.srcW * c.srcH * 4);
    auto dst = Pixels(c.dstW * c.dstH * 4);
    const long long outPixels = static_cast<long long>(c.dstW) * c.dstH;
    const int iters =
        static_cast<int>(std::max<long long>(20, 80'000'000LL / outPixels));

    const auto scalar = &eacp::simd::backends::resizeBilinear_scalar;
    const auto simd = baselineResize();

    const auto scalarMs = timeMs(
        iters,
        src,
        dst.data(),
        [&] { scalar(src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH); });
    const auto simdMs = timeMs(
        iters,
        src,
        dst.data(),
        [&] { simd(src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH); });

    std::printf("  %4dx%-4d -> %4dx%-4d | scalar %8.3f ms %7.1f Mpix/s | "
                "%-4s %8.3f ms %7.1f Mpix/s | %5.2fx\n",
                c.srcW,
                c.srcH,
                c.dstW,
                c.dstH,
                scalarMs,
                megaPixelsPerSec(outPixels, scalarMs),
                baselineName(),
                simdMs,
                megaPixelsPerSec(outPixels, simdMs),
                scalarMs / simdMs);
}

void benchSwap(long long pixels)
{
    auto src = makeBuffer(static_cast<int>(pixels) * 4);
    auto dst = Pixels(static_cast<int>(pixels) * 4);
    const auto count = static_cast<std::size_t>(pixels);
    const int iters =
        static_cast<int>(std::max<long long>(20, 400'000'000LL / pixels));

    const auto scalarMs = timeMs(iters,
                                 src,
                                 dst.data(),
                                 [&]
                                 {
                                     eacp::simd::backends::swapRedBlue_scalar(
                                         src.data(), dst.data(), count);
                                 });
    const auto simd = baselineSwap();
    const auto simdMs =
        timeMs(iters, src, dst.data(), [&] { simd(src.data(), dst.data(), count); });

    std::printf("  %5.1f Mpix | scalar %8.3f ms %7.1f Mpix/s | "
                "%-4s %8.3f ms %7.1f Mpix/s | %5.2fx",
                static_cast<double>(pixels) / 1'000'000.0,
                scalarMs,
                megaPixelsPerSec(pixels, scalarMs),
                baselineName(),
                simdMs,
                megaPixelsPerSec(pixels, simdMs),
                scalarMs / simdMs);

#if defined(EACP_SIMD_HAS_AVX2)
    if (eacp::simd::cpu::hasAvx2Fma())
    {
        const auto avx2Ms = timeMs(iters,
                                   src,
                                   dst.data(),
                                   [&]
                                   {
                                       eacp::simd::backends::swapRedBlue_avx2(
                                           src.data(), dst.data(), count);
                                   });
        std::printf(" | avx2 %8.3f ms %7.1f Mpix/s | %5.2fx",
                    avx2Ms,
                    megaPixelsPerSec(pixels, avx2Ms),
                    scalarMs / avx2Ms);
    }
#endif
    std::printf("\n");
}
} // namespace

int main()
{
#if defined(_MSC_VER) && !defined(NDEBUG)
    std::printf("Note: MSVC Debug -- the kernels are /O2 but this benchmark "
                "harness is /Od; build Release for the cleanest numbers.\n\n");
#endif

    std::printf("eacp-simd benchmark | baseline SIMD = %s", baselineName());
#if defined(EACP_SIMD_HAS_AVX2)
    std::printf(", avx2 %s", eacp::simd::cpu::hasAvx2Fma() ? "available" : "absent");
#endif
    std::printf("\n\nresizeBilinear:\n");

    const ResizeConfig resizes[] = {
        {1920, 1080, 640, 360}, // 3x downscale
        {1280, 720, 1280, 720}, // identity
        {640, 360, 1920, 1080}, // 3x upscale
        {1280, 720, 213, 120}, // thumbnail
        {512, 512, 1024, 1024}, // 2x upscale
    };
    for (const auto& c: resizes)
        benchResize(c);

    std::printf("\nswapRedBlue:\n");
    for (long long pixels: {1'000'000LL, 8'000'000LL})
        benchSwap(pixels);

    std::printf("\nchecksum %llu\n", static_cast<unsigned long long>(gSink));
    return 0;
}
