#pragma once

#include <cstddef>
#include <cstdint>

// eacp-simd: a small, self-contained portable SIMD layer.
//
// This is the public, portable surface. It carries NO architecture or feature
// conditionals -- every one of those lives in the implementation (and in the
// internal <eacp/SIMD/Backends.h>). Each call selects the fastest backend
// available on the host at runtime, once, behind a function pointer.
namespace eacp::simd
{

// --- Image kernels ---

// Swap the red and blue channels of `pixelCount` tightly-packed 8-bit RGBA
// pixels (RGBA <-> BGRA), writing to `out`. `in` and `out` may be equal
// (in place) but must not otherwise overlap.
void swapRedBlue(const std::uint8_t* in, std::uint8_t* out, std::size_t pixelCount);

// Resize a tightly-packed RGBA8 image with bilinear sampling, half-pixel-center
// mapping and edge clamping (OpenCV semantics). `dst` holds dstW*dstH*4 bytes.
// Caller guarantees src/dst dimensions are positive and buffers correctly sized.
void resizeBilinear(const std::uint8_t* src,
                    int srcWidth,
                    int srcHeight,
                    std::uint8_t* dst,
                    int dstWidth,
                    int dstHeight);

// --- Elementwise float-array primitives ---
//
// Each processes `count` floats. `out` may alias an input (safe, elementwise).
// SIMD-accelerated and deterministic: being elementwise, the result is identical
// at any vector width and on every build.

// out[i] = a[i] + b[i]
void add(const float* a, const float* b, float* out, std::size_t count);

// out[i] = a[i] - b[i]
void subtract(const float* a, const float* b, float* out, std::size_t count);

// out[i] = a[i] * b[i]
void multiply(const float* a, const float* b, float* out, std::size_t count);

// out[i] = a[i] * scalar
void multiplyByScalar(const float* a, float scalar, float* out, std::size_t count);

// out[i] = a[i] * b[i] + c[i]
void multiplyAdd(
    const float* a, const float* b, const float* c, float* out, std::size_t count);

} // namespace eacp::simd
