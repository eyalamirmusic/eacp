#pragma once

#include <eacp/SIMD/SIMD.h>

#include <algorithm>
#include <concepts>
#include <cstddef>

// Header-only, buffer-level conveniences over the raw float-array primitives in
// SIMD.h. Anything contiguous with data()/size() qualifies (EA::Vector,
// std::vector, std::span, std::array, ...), so call sites can write
// `multiply(buffer, gain)` instead of spelling out pointers and counts.
//
// Every helper is elementwise and in place on its first argument. When the
// buffers disagree on size, the common prefix is processed -- never past the
// end of the shortest one.
namespace eacp::simd
{

template <typename B>
concept FloatBuffer = requires(const B& b) {
    { b.data() } -> std::convertible_to<const float*>;
    { b.size() } -> std::convertible_to<std::size_t>;
};

template <typename B>
concept MutableFloatBuffer = FloatBuffer<B> && requires(B& b) {
    { b.data() } -> std::convertible_to<float*>;
};

template <FloatBuffer... Buffers>
std::size_t commonCount(const Buffers&... buffers)
{
    return std::min({static_cast<std::size_t>(buffers.size())...});
}

// dst[i] += src[i]
template <MutableFloatBuffer Dst, FloatBuffer Src>
void add(Dst& dst, const Src& src)
{
    add(dst.data(), src.data(), dst.data(), commonCount(dst, src));
}

// dst[i] -= src[i]
template <MutableFloatBuffer Dst, FloatBuffer Src>
void subtract(Dst& dst, const Src& src)
{
    subtract(dst.data(), src.data(), dst.data(), commonCount(dst, src));
}

// dst[i] *= src[i]
template <MutableFloatBuffer Dst, FloatBuffer Src>
void multiply(Dst& dst, const Src& src)
{
    multiply(dst.data(), src.data(), dst.data(), commonCount(dst, src));
}

// dst[i] *= value
template <MutableFloatBuffer Dst>
void multiply(Dst& dst, float value)
{
    multiplyByScalar(
        dst.data(), value, dst.data(), static_cast<std::size_t>(dst.size()));
}

// dst[i] += a[i] * b[i]
template <MutableFloatBuffer Dst, FloatBuffer A, FloatBuffer B>
void multiplyAdd(Dst& dst, const A& a, const B& b)
{
    multiplyAdd(a.data(), b.data(), dst.data(), dst.data(), commonCount(dst, a, b));
}

// dst[i] += src[i] * value
template <MutableFloatBuffer Dst, FloatBuffer Src>
void multiplyAdd(Dst& dst, const Src& src, float value)
{
    multiplyAdd(src.data(), value, dst.data(), dst.data(), commonCount(dst, src));
}

// dst[i] += t * (target[i] - dst[i])
template <MutableFloatBuffer Dst, FloatBuffer Target>
void lerp(Dst& dst, const Target& target, float t)
{
    lerp(dst.data(), target.data(), t, dst.data(), commonCount(dst, target));
}

// sum(src[i]^2)
template <FloatBuffer Src>
double sumOfSquares(const Src& src)
{
    return sumOfSquares(src.data(), static_cast<std::size_t>(src.size()));
}

// max(|src[i]|)
template <FloatBuffer Src>
float peakAbs(const Src& src)
{
    return peakAbs(src.data(), static_cast<std::size_t>(src.size()));
}

} // namespace eacp::simd
