#pragma once

#include <cstddef>
#include <cstdint>

// Public, portable surface: no architecture or feature conditionals. Each call
// selects the fastest backend available on the host at runtime, once, behind a
// function pointer.
namespace eacp::simd
{

// Swaps red and blue of `pixelCount` tightly-packed 8-bit RGBA pixels. `in` and
// `out` may be equal (in place) but must not otherwise overlap.
void swapRedBlue(const std::uint8_t* in, std::uint8_t* out, std::size_t pixelCount);

// BGRA8 to tightly-packed RGBA8 in one pass, dropping trailing row padding.
// `src` rows are `srcBytesPerRow` apart (>= width*4); `dst` holds
// width*height*4 bytes with no padding.
void convertBgraToRgba(const std::uint8_t* src,
                       std::size_t srcBytesPerRow,
                       std::uint8_t* dst,
                       int width,
                       int height);

// Bilinear sampling, half-pixel-center mapping and edge clamping (OpenCV
// semantics). `dst` holds dstW*dstH*4 bytes. Caller guarantees positive
// dimensions and correctly sized buffers.
void resizeBilinear(const std::uint8_t* src,
                    int srcWidth,
                    int srcHeight,
                    std::uint8_t* dst,
                    int dstWidth,
                    int dstHeight);

// `inverse2x3` holds the six row-major coefficients [m0 m1 m2; m3 m4 m5]: each
// destination pixel (dx, dy) samples the source at bilinearly filtered,
// edge-clamped (m0*dx + m1*dy + m2, m3*dx + m4*dy + m5).
void warpAffineInverse(const std::uint8_t* src,
                       int srcWidth,
                       int srcHeight,
                       const float* inverse2x3,
                       std::uint8_t* dst,
                       int dstWidth,
                       int dstHeight);

// Crops a width x height region at (x, y) and mirrors it horizontally in one
// pass. The caller guarantees the region lies within the source.
void mirroredCrop(const std::uint8_t* src,
                  int srcWidth,
                  int x,
                  int y,
                  int width,
                  int height,
                  std::uint8_t* dst);

// Each processes `count` floats. `out` may alias an input (safe, elementwise),
// and being elementwise the result is identical at any vector width and on
// every build.

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

// out[i] = a[i] * b + c[i]; with out == c, accumulates in place.
void multiplyAdd(
    const float* a, float b, const float* c, float* out, std::size_t count);

// out[i] = a[i] + t * (b[i] - a[i])
void lerp(const float* a, const float* b, float t, float* out, std::size_t count);

// Reductions use a fixed four-lane interleave, so they are deterministic across
// builds and architectures but NOT bit-equal to a naive sequential loop.

// sum(a[i]^2), accumulated in double. 0.0 when count == 0.
double sumOfSquares(const float* a, std::size_t count);

// max(|a[i]|), 0.f when count == 0. Order-independent, so it matches a
// sequential loop exactly.
float peakAbs(const float* a, std::size_t count);

} // namespace eacp::simd
