#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/SIMD.h>

#include <ea_data_structures/ea_data_structures.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

// Micro-benchmark comparing the scalar reference against the active SIMD backend
// for the eacp-simd kernels, plus the float-array primitives (auto-vectorized vs
// a non-vectorized scalar reference). eacp-simd and this harness are always built
// -O3, so any build config is fine.

// Disable auto-vectorization for the array primitives' scalar reference, so the
// comparison shows the vectorization win rather than vectorized-vs-vectorized.
#if defined(__clang__)
#define EACP_NO_VECTORIZE                                                           \
    _Pragma("clang loop vectorize(disable) interleave(disable)")
#elif defined(_MSC_VER)
#define EACP_NO_VECTORIZE __pragma(loop(no_vector))
#else
#define EACP_NO_VECTORIZE
#endif

namespace
{
using Pixels = EA::Vector<std::uint8_t>;
using Floats = EA::Vector<float>;
using ResizeFn = void (*)(const std::uint8_t*, int, int, std::uint8_t*, int, int);
using SwapFn = void (*)(const std::uint8_t*, std::uint8_t*, std::size_t);
using WarpFn =
    void (*)(const std::uint8_t*, int, int, const float*, std::uint8_t*, int, int);

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

Floats makeFloats(int count, int stride, int offset)
{
    auto data = Floats(count);
    for (int i = 0; i < count; ++i)
        data[i] = static_cast<float>((i % stride) + offset);
    return data;
}

std::uint8_t* bytes(Floats& v)
{
    return reinterpret_cast<std::uint8_t*>(v.data());
}

// Returns milliseconds per iteration. One input byte is perturbed each iteration
// so the call is not loop-invariant; one output byte is observed so the stores
// are not dead. Both are O(1) and negligible against a whole-buffer op.
template <class Run>
double timeMs(int iters,
              std::uint8_t* perturb,
              int perturbBytes,
              const std::uint8_t* observe,
              Run&& run)
{
    run(); // warm up caches and the dispatch pointer

    std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
    {
        perturb[i % perturbBytes] ^= static_cast<std::uint8_t>(i + 1);
        run();
        sink += observe[0];
    }
    const auto end = std::chrono::steady_clock::now();

    gSink ^= sink;
    return std::chrono::duration<double, std::milli>(end - start).count()
           / static_cast<double>(iters);
}

double perSec(long long items, double ms)
{
    return static_cast<double>(items) / (ms * 1000.0);
}

void report(
    const char* name, long long items, double ms, double scalarMs, const char* unit)
{
    std::printf(" | %-5s %8.3f ms %7.1f %s %5.2fx",
                name,
                ms,
                perSec(items, ms),
                unit,
                scalarMs / ms);
}

// Non-vectorized scalar references for the array primitives.
void scalarAdd(const float* a, const float* b, float* out, std::size_t n)
{
    EACP_NO_VECTORIZE
    for (std::size_t i = 0; i < n; ++i)
        out[i] = a[i] + b[i];
}

void scalarSubtract(const float* a, const float* b, float* out, std::size_t n)
{
    EACP_NO_VECTORIZE
    for (std::size_t i = 0; i < n; ++i)
        out[i] = a[i] - b[i];
}

void scalarMultiply(const float* a, const float* b, float* out, std::size_t n)
{
    EACP_NO_VECTORIZE
    for (std::size_t i = 0; i < n; ++i)
        out[i] = a[i] * b[i];
}

void scalarMultiplyByScalar(const float* a, float s, float* out, std::size_t n)
{
    EACP_NO_VECTORIZE
    for (std::size_t i = 0; i < n; ++i)
        out[i] = a[i] * s;
}

void scalarMultiplyAdd(
    const float* a, const float* b, const float* c, float* out, std::size_t n)
{
    EACP_NO_VECTORIZE
    for (std::size_t i = 0; i < n; ++i)
        out[i] = a[i] * b[i] + c[i];
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

WarpFn baselineWarp()
{
#if defined(__x86_64__) || defined(_M_X64)
    return &eacp::simd::backends::warpAffineInverse_sse2;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return &eacp::simd::backends::warpAffineInverse_neon;
#else
    return &eacp::simd::backends::warpAffineInverse_scalar;
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
        src.data(),
        src.size(),
        dst.data(),
        [&] { scalar(src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH); });
    const auto simdMs = timeMs(
        iters,
        src.data(),
        src.size(),
        dst.data(),
        [&] { simd(src.data(), c.srcW, c.srcH, dst.data(), c.dstW, c.dstH); });

    std::printf("  %4dx%-4d -> %4dx%-4d | scalar %8.3f ms %7.1f Mpix/s",
                c.srcW,
                c.srcH,
                c.dstW,
                c.dstH,
                scalarMs,
                perSec(outPixels, scalarMs));
    report(baselineName(), outPixels, simdMs, scalarMs, "Mpix/s");
    std::printf("\n");
}

void benchWarp(const ResizeConfig& c)
{
    auto src = makeBuffer(c.srcW * c.srcH * 4);
    auto dst = Pixels(c.dstW * c.dstH * 4);
    // A rotate + scale + translate inverse matrix (the values don't affect timing).
    const float m[6] = {0.8f, -0.2f, 8.f, 0.2f, 0.8f, 5.f};
    const long long outPixels = static_cast<long long>(c.dstW) * c.dstH;
    const int iters =
        static_cast<int>(std::max<long long>(20, 80'000'000LL / outPixels));

    const auto scalar = &eacp::simd::backends::warpAffineInverse_scalar;
    const auto simd = baselineWarp();

    const auto scalarMs = timeMs(
        iters,
        src.data(),
        src.size(),
        dst.data(),
        [&] { scalar(src.data(), c.srcW, c.srcH, m, dst.data(), c.dstW, c.dstH); });
    const auto simdMs = timeMs(
        iters,
        src.data(),
        src.size(),
        dst.data(),
        [&] { simd(src.data(), c.srcW, c.srcH, m, dst.data(), c.dstW, c.dstH); });

    std::printf("  %4dx%-4d -> %4dx%-4d | scalar %8.3f ms %7.1f Mpix/s",
                c.srcW,
                c.srcH,
                c.dstW,
                c.dstH,
                scalarMs,
                perSec(outPixels, scalarMs));
    report(baselineName(), outPixels, simdMs, scalarMs, "Mpix/s");
    std::printf("\n");
}

void benchSwap(long long pixels)
{
    auto src = makeBuffer(static_cast<int>(pixels) * 4);
    auto dst = Pixels(static_cast<int>(pixels) * 4);
    const auto count = static_cast<std::size_t>(pixels);
    const int iters =
        static_cast<int>(std::max<long long>(20, 400'000'000LL / pixels));

    const auto scalarMs = timeMs(iters,
                                 src.data(),
                                 src.size(),
                                 dst.data(),
                                 [&]
                                 {
                                     eacp::simd::backends::swapRedBlue_scalar(
                                         src.data(), dst.data(), count);
                                 });
    const auto simd = baselineSwap();
    const auto simdMs = timeMs(iters,
                               src.data(),
                               src.size(),
                               dst.data(),
                               [&] { simd(src.data(), dst.data(), count); });

    std::printf("  %5.1f Mpix | scalar %8.3f ms %7.1f Mpix/s",
                static_cast<double>(pixels) / 1'000'000.0,
                scalarMs,
                perSec(pixels, scalarMs));
    report(baselineName(), pixels, simdMs, scalarMs, "Mpix/s");

#if defined(EACP_SIMD_HAS_AVX2)
    if (eacp::simd::cpu::hasAvx2Fma())
    {
        const auto avx2Ms = timeMs(iters,
                                   src.data(),
                                   src.size(),
                                   dst.data(),
                                   [&]
                                   {
                                       eacp::simd::backends::swapRedBlue_avx2(
                                           src.data(), dst.data(), count);
                                   });
        report("avx2", pixels, avx2Ms, scalarMs, "Mpix/s");
    }
#endif
    std::printf("\n");
}

void benchArrayOps(int count)
{
    auto a = makeFloats(count, 17, 1);
    auto b = makeFloats(count, 23, 1);
    auto c = makeFloats(count, 5, 1);
    auto out = Floats(count);
    const auto n = static_cast<std::size_t>(count);
    const int iters = std::max(20, 400'000'000 / count);

    std::printf("  %d elements:\n", count);

    const auto row = [&](const char* name, auto scalarRun, auto simdRun)
    {
        const auto scalarMs =
            timeMs(iters, bytes(a), a.size() * 4, bytes(out), scalarRun);
        const auto simdMs =
            timeMs(iters, bytes(a), a.size() * 4, bytes(out), simdRun);
        std::printf("    %-16s | scalar %8.3f ms %7.1f Melem/s",
                    name,
                    scalarMs,
                    perSec(count, scalarMs));
        report(baselineName(), count, simdMs, scalarMs, "Melem/s");
        std::printf("\n");
    };

    row(
        "add",
        [&] { scalarAdd(a.data(), b.data(), out.data(), n); },
        [&] { eacp::simd::add(a.data(), b.data(), out.data(), n); });
    row(
        "subtract",
        [&] { scalarSubtract(a.data(), b.data(), out.data(), n); },
        [&] { eacp::simd::subtract(a.data(), b.data(), out.data(), n); });
    row(
        "multiply",
        [&] { scalarMultiply(a.data(), b.data(), out.data(), n); },
        [&] { eacp::simd::multiply(a.data(), b.data(), out.data(), n); });
    row(
        "multiplyByScalar",
        [&] { scalarMultiplyByScalar(a.data(), 3.f, out.data(), n); },
        [&] { eacp::simd::multiplyByScalar(a.data(), 3.f, out.data(), n); });
    row(
        "multiplyAdd",
        [&] { scalarMultiplyAdd(a.data(), b.data(), c.data(), out.data(), n); },
        [&]
        { eacp::simd::multiplyAdd(a.data(), b.data(), c.data(), out.data(), n); });
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
    for (const auto& cfg: resizes)
        benchResize(cfg);

    std::printf("\nwarpAffineInverse:\n");
    for (const auto& cfg: resizes)
        benchWarp(cfg);

    std::printf("\nswapRedBlue:\n");
    for (long long pixels: {1'000'000LL, 8'000'000LL})
        benchSwap(pixels);

    std::printf("\narray primitives (non-vectorized scalar vs %s):\n",
                baselineName());
    benchArrayOps(4096); // cache-resident: shows the vectorization win
    benchArrayOps(4'000'000); // larger than cache: memory-bandwidth-bound

    std::printf("\nchecksum %llu\n", static_cast<unsigned long long>(gSink));
    return 0;
}
