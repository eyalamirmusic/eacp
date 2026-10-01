#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

// One word per lane per component; loops are branch-free so they vectorise (D5).

namespace eacp::GPU::CpuCompute
{
using Word = std::uint32_t;

namespace Lanes
{
inline float toFloat(Word word)
{
    return std::bit_cast<float>(word);
}

inline Word toWord(float value)
{
    return std::bit_cast<Word>(value);
}

inline std::int32_t toSigned(Word word)
{
    return std::bit_cast<std::int32_t>(word);
}

inline Word fromSigned(std::int32_t value)
{
    return std::bit_cast<Word>(value);
}

inline Word maskOf(bool condition)
{
    return Word {0} - static_cast<Word>(condition);
}

inline void copy(Word* out, const Word* in, int count)
{
    std::memcpy(out, in, sizeof(Word) * static_cast<std::size_t>(count));
}

inline void fill(Word* out, Word value, int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = value;
}

inline bool anyActive(const Word* mask, int count)
{
    auto combined = Word {0};

    for (auto lane = 0; lane < count; ++lane)
        combined |= mask[lane];

    return combined != 0;
}

inline void blend(Word* out, const Word* value, const Word* mask, int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = (value[lane] & mask[lane]) | (out[lane] & ~mask[lane]);
}

inline void intersect(Word* out, const Word* a, const Word* b, int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = a[lane] & b[lane];
}

inline void intersectComplement(Word* out, const Word* a, const Word* b, int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = a[lane] & ~b[lane];
}

inline void clearWhere(Word* out, const Word* cleared, int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] &= ~cleared[lane];
}

inline void select(Word* out,
                   const Word* mask,
                   const Word* whenTrue,
                   const Word* whenFalse,
                   int count)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = (whenTrue[lane] & mask[lane]) | (whenFalse[lane] & ~mask[lane]);
}

template <typename Function>
void mapFloats(Word* out, const Word* a, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = toWord(function(toFloat(a[lane])));
}

template <typename Function>
void zipFloats(Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = toWord(function(toFloat(a[lane]), toFloat(b[lane])));
}

template <typename Function>
void zipFloats3(Word* out,
                const Word* a,
                const Word* b,
                const Word* c,
                int count,
                Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] =
            toWord(function(toFloat(a[lane]), toFloat(b[lane]), toFloat(c[lane])));
}

template <typename Function>
void compareFloats(
    Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = maskOf(function(toFloat(a[lane]), toFloat(b[lane])));
}

template <typename Function>
void mapWords(Word* out, const Word* a, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = function(a[lane]);
}

template <typename Function>
void zipWords(Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = function(a[lane], b[lane]);
}

template <typename Function>
void zipSigned(Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = fromSigned(function(toSigned(a[lane]), toSigned(b[lane])));
}

template <typename Function>
void compareWords(
    Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = maskOf(function(a[lane], b[lane]));
}

template <typename Function>
void compareSigned(
    Word* out, const Word* a, const Word* b, int count, Function function)
{
    for (auto lane = 0; lane < count; ++lane)
        out[lane] = maskOf(function(toSigned(a[lane]), toSigned(b[lane])));
}
} // namespace Lanes
} // namespace eacp::GPU::CpuCompute
