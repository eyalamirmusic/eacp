#include "Cpu.h"
#include "../Common.h"

// Compiled at baseline (no -mavx2): must run on any CPU to decide whether the
// AVX2 path is safe to call.

#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace
{

std::uint64_t readXcr0()
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    // Inline asm avoids depending on _xgetbv being enabled at baseline; only
    // reached after confirming OSXSAVE, so xgetbv is available.
    std::uint32_t eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#endif
}

bool detectAvx2Fma() noexcept
{
    // eax/edx exist solely as __get_cpuid_count out-params, so they live on the
    // non-MSVC path only; declaring them on the MSVC path would warn C4189.
    std::uint32_t ebx = 0, ecx = 0;

#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, 1, 0);
    ecx = static_cast<std::uint32_t>(regs[2]);
#else
    std::uint32_t eax = 0, edx = 0;
    if (!__get_cpuid_count(1, 0, &eax, &ebx, &ecx, &edx))
        return false;
#endif

    const bool osUsesXsave = (ecx & (1u << 27)) != 0; // OSXSAVE
    const bool hasFma = (ecx & (1u << 12)) != 0; // FMA3
    if (!osUsesXsave || !hasFma)
        return false;

    // The OS must have enabled XMM (bit 1) and YMM (bit 2) state saving.
    if ((readXcr0() & 0x6u) != 0x6u)
        return false;

#if defined(_MSC_VER)
    __cpuidex(regs, 7, 0);
    ebx = static_cast<std::uint32_t>(regs[1]);
#else
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return false;
#endif

    return (ebx & (1u << 5)) != 0; // AVX2
}

} // namespace

namespace eacp::simd::cpu
{

bool hasAvx2Fma() noexcept
{
    static const bool value = detectAvx2Fma();
    return value;
}

} // namespace eacp::simd::cpu

#else

namespace eacp::simd::cpu
{

bool hasAvx2Fma() noexcept
{
    return false;
}

} // namespace eacp::simd::cpu

#endif
