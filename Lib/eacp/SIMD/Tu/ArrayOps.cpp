#include "../SIMD.h"
#include "../Common.h"

#include <algorithm>

// Plain loops the compiler auto-vectorizes to the module's target ISA at -O3;
// memory-bandwidth-bound, so runtime-dispatched AVX2 would add nothing.
// multiplyAdd is non-fused (-ffp-contract=off), matching the image kernels.
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

// Four accumulators in a fixed interleave: explicit reassociation lets the
// compiler use vector lanes without fast-math, while keeping the accumulation
// order (and so the result) identical on every build.

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
