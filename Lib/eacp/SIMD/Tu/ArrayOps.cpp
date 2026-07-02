#include <eacp/SIMD/SIMD.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

// Elementwise float-array primitives. These are plain loops that the compiler
// auto-vectorizes to the module's target ISA at -O3 (SSE2 on x86-64, NEON on
// arm64). They are memory-bandwidth-bound, so a runtime-dispatched AVX2 variant
// would add essentially nothing (the same lesson as swapRedBlue at ~1.0x); a
// wider, dispatched path can be introduced later if a compute-bound primitive
// needs it. Being elementwise, the results are deterministic on every build.
//
// multiplyAdd is intentionally non-fused (the module is built -ffp-contract=off),
// keeping it consistent with the bit-exact image kernels.
namespace eacp::simd
{

void add(const float* a, const float* b, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] + b[i];
}

void subtract(const float* a, const float* b, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] - b[i];
}

void multiply(const float* a, const float* b, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] * b[i];
}

void multiplyByScalar(const float* a, float scalar, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] * scalar;
}

void multiplyAdd(
    const float* a, const float* b, const float* c, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] * b[i] + c[i];
}

void multiplyAdd(
    const float* a, float b, const float* c, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] * b + c[i];
}

void lerp(const float* a, const float* b, float t, float* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out[i] = a[i] + t * (b[i] - a[i]);
}

// The reductions below keep four independent accumulators in a fixed
// interleave: the explicit reassociation is what lets the compiler map them
// onto vector lanes without fast-math, while keeping the accumulation order
// (and therefore the result) identical on every build.

double sumOfSquares(const float* a, std::size_t count)
{
    auto acc0 = 0.0;
    auto acc1 = 0.0;
    auto acc2 = 0.0;
    auto acc3 = 0.0;

    auto i = std::size_t {0};
    for (; i + 4 <= count; i += 4)
    {
        acc0 += (double) a[i + 0] * (double) a[i + 0];
        acc1 += (double) a[i + 1] * (double) a[i + 1];
        acc2 += (double) a[i + 2] * (double) a[i + 2];
        acc3 += (double) a[i + 3] * (double) a[i + 3];
    }

    for (; i < count; ++i)
        acc0 += (double) a[i] * (double) a[i];

    return (acc0 + acc1) + (acc2 + acc3);
}

float peakAbs(const float* a, std::size_t count)
{
    auto max0 = 0.f;
    auto max1 = 0.f;
    auto max2 = 0.f;
    auto max3 = 0.f;

    auto i = std::size_t {0};
    for (; i + 4 <= count; i += 4)
    {
        max0 = std::max(max0, std::abs(a[i + 0]));
        max1 = std::max(max1, std::abs(a[i + 1]));
        max2 = std::max(max2, std::abs(a[i + 2]));
        max3 = std::max(max3, std::abs(a[i + 3]));
    }

    for (; i < count; ++i)
        max0 = std::max(max0, std::abs(a[i]));

    return std::max(std::max(max0, max1), std::max(max2, max3));
}

} // namespace eacp::simd
