#pragma once

namespace eacp::simd::cpu
{

// True when the CPU supports AVX2 and FMA and the OS has enabled YMM state, so
// VEX-encoded 256-bit code is safe to run. Cached; false on non-x86 targets.
bool hasAvx2Fma() noexcept;

} // namespace eacp::simd::cpu
