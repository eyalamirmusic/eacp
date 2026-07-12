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

// Convert a BGRA8 camera frame to tightly-packed RGBA8 in one pass: swap
// red/blue and drop any trailing row padding. `src` rows are `srcBytesPerRow`
// apart (>= width*4); `dst` holds width*height*4 bytes with no padding. This
// lives in the SIMD module so its per-byte work is always built at the platform
// optimization maximum, independent of the (possibly unoptimized) caller.
void convertBgraToRgba(const std::uint8_t* src,
                       std::size_t srcBytesPerRow,
                       std::uint8_t* dst,
                       int width,
                       int height);

// Resize a tightly-packed RGBA8 image with bilinear sampling, half-pixel-center
// mapping and edge clamping (OpenCV semantics). `dst` holds dstW*dstH*4 bytes.
// Caller guarantees src/dst dimensions are positive and buffers correctly sized.
void resizeBilinear(const std::uint8_t* src,
                    int srcWidth,
                    int srcHeight,
                    std::uint8_t* dst,
                    int dstWidth,
                    int dstHeight);

// Inverse affine warp of a tightly-packed RGBA8 image with bilinear sampling and
// edge clamping. `inverse2x3` points to the six row-major coefficients of the
// 2x3 inverse matrix ([m0 m1 m2; m3 m4 m5]): each destination pixel (dx, dy)
// samples the source at (m0*dx + m1*dy + m2, m3*dx + m4*dy + m5). `dst` holds
// dstW*dstH*4 bytes.
void warpAffineInverse(const std::uint8_t* src,
                       int srcWidth,
                       int srcHeight,
                       const float* inverse2x3,
                       std::uint8_t* dst,
                       int dstWidth,
                       int dstHeight);

// Crop a width x height region at (x, y) of a tightly-packed RGBA8 image and
// mirror it horizontally in one pass. `dst` holds width*height*4 bytes. The
// caller guarantees the crop region lies within the source.
void mirroredCrop(const std::uint8_t* src,
                  int srcWidth,
                  int x,
                  int y,
                  int width,
                  int height,
                  std::uint8_t* dst);

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

// out[i] = a[i] * b + c[i] -- the axpy shape; with out == c it accumulates a
// scaled array in place (out[i] += a[i] * b).
void multiplyAdd(
    const float* a, float b, const float* c, float* out, std::size_t count);

// out[i] = a[i] + t * (b[i] - a[i])
void lerp(const float* a, const float* b, float t, float* out, std::size_t count);

// --- Float-array reductions ---
//
// Unlike the elementwise primitives, a reduction has an accumulation order.
// These use a fixed four-lane interleave (explicitly reassociated so the
// compiler can vectorize them without fast-math), so the result is still
// deterministic — identical on every build and architecture — but it is NOT
// bit-equal to a naive sequential loop over the same data.

// Returns sum(a[i]^2), accumulated in double for precision. 0.0 when count == 0.
double sumOfSquares(const float* a, std::size_t count);

// Returns max(|a[i]|), 0.f when count == 0. Max is order-independent, so this
// one matches a sequential loop exactly.
float peakAbs(const float* a, std::size_t count);

} // namespace eacp::simd
