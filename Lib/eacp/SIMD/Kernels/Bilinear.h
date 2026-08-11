#pragma once

#include "../Common.h"

namespace eacp::simd::kernels
{

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Combines four RGBA taps with four weights in a fixed per-channel reduction
// order, so it is bit-exact across every backend when the TU is compiled
// -ffp-contract=off.
template <class B>
void blendTaps(const std::uint8_t* p00,
               const std::uint8_t* p10,
               const std::uint8_t* p01,
               const std::uint8_t* p11,
               float w00,
               float w10,
               float w01,
               float w11,
               std::uint8_t* out)
{
    using F4 = typename B::F4;
    const auto acc = F4::broadcast(w00) * F4::loadPixel(p00)
                     + F4::broadcast(w10) * F4::loadPixel(p10)
                     + F4::broadcast(w01) * F4::loadPixel(p01)
                     + F4::broadcast(w11) * F4::loadPixel(p11);
    F4::storePixel(out, acc);
}

} // namespace eacp::simd::kernels
