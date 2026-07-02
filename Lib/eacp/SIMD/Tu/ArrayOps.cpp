#include <eacp/SIMD/SIMD.h>

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

} // namespace eacp::simd
