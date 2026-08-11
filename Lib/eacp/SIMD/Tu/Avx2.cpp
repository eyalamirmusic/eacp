// The ONLY translation unit compiled with an ISA flag (-mavx2 -mfma /
// /arch:AVX2, per-source by CMake), and reached only through a function pointer
// once cpu::hasAvx2Fma() is true. The target attribute stops LTO inlining it.

#if defined(__x86_64__) || defined(_M_X64)

#include "../Backend/Avx2.h"
#include "../Backends.h"
#include "../Kernels/SwapRedBlue.h"

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
