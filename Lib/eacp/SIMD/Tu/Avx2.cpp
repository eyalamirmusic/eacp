// AVX2 backend entry points. This is the ONLY translation unit compiled with an
// ISA flag (-mavx2 -mfma / /arch:AVX2, applied per-source by CMake). It is built
// only for single-arch x86-64; the whole body is arch-guarded so a stray
// compile on another slice degrades to an empty object.
//
// The dispatcher reaches these symbols only through a function pointer, and only
// after cpu::hasAvx2Fma() returns true. The target attribute is a secondary
// guard that keeps the compiler from inlining this AVX2 code into a baseline
// caller under LTO (IPO is also disabled for this target in CMake).

#if defined(__x86_64__) || defined(_M_X64)

#include <eacp/SIMD/Backend/Avx2.h>
#include <eacp/SIMD/Backends.h>
#include <eacp/SIMD/Kernels/SwapRedBlue.h>

namespace eacp::simd::backends
{

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
void swapRedBlue_avx2(const std::uint8_t* in,
                      std::uint8_t* out,
                      std::size_t pixelCount)
{
    kernels::swapRedBlueImpl<backend::Avx2>(in, out, pixelCount);
}

} // namespace eacp::simd::backends

#endif
