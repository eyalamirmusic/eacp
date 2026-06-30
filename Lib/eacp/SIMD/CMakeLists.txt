add_library(eacp-simd STATIC)
target_include_directories(eacp-simd PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

target_sources(eacp-simd PRIVATE
        Dispatch/Cpu.cpp
        Dispatch/Dispatch.cpp
        Tu/Scalar.cpp
        Tu/Baseline.cpp)

# Architecture detection: prefer the Apple multi-arch list (which can hold both
# arm64 and x86_64 for a universal build), otherwise the single system processor.
set(_eacp_simd_archs "${CMAKE_OSX_ARCHITECTURES}")
if (NOT _eacp_simd_archs)
    set(_eacp_simd_archs "${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(_eacp_simd_x86 OFF)
set(_eacp_simd_arm OFF)
foreach (_arch IN LISTS _eacp_simd_archs)
    if (_arch MATCHES "^(x86_64|amd64|AMD64|x64)$")
        set(_eacp_simd_x86 ON)
    elseif (_arch MATCHES "^(arm64|arm64e|aarch64)$")
        set(_eacp_simd_arm ON)
    endif ()
endforeach ()

# The AVX2 backend is a separately compiled, runtime-selected translation unit.
# It exists only for a single-arch x86-64 build: a per-source -mavx2 cannot be
# expressed per-slice in an Apple universal (arm64;x86_64) compile, so a
# universal x86 slice falls back to the SSE2 baseline instead.
if (_eacp_simd_x86 AND NOT _eacp_simd_arm)
    target_sources(eacp-simd PRIVATE Tu/Avx2.cpp)
    # PUBLIC so consumers (tests, graphics) see a consistent declaration of the
    # AVX2 entry points and the dispatcher references a symbol that exists.
    target_compile_definitions(eacp-simd PUBLIC EACP_SIMD_HAS_AVX2=1)

    if (MSVC)
        set(_eacp_avx2_flags /arch:AVX2)
    else ()
        set(_eacp_avx2_flags -mavx2 -mfma)
    endif ()

    # The ISA flag must hit only this one TU; the per-source COMPILE_OPTIONS also
    # excludes it from the unity blob, but mark it explicitly for intent.
    set_source_files_properties(Tu/Avx2.cpp PROPERTIES
            COMPILE_OPTIONS "${_eacp_avx2_flags}"
            SKIP_UNITY_BUILD_INCLUSION ON)
endif ()

set_default_target_setting(eacp-simd)

# Disable IPO/LTO for this target. set_default_target_setting turns IPO on for
# every target, but the runtime AVX2 dispatch is a deliberately non-inlinable
# boundary: under LTO the optimizer could hoist AVX2 code into a baseline caller
# (on MSVC /LTCG there is no per-function target attribute to prevent it), which
# would fault on pre-AVX2 CPUs. Nothing is lost -- the boundary is non-inlinable
# by design -- and this target's native objects still link fine against LTO ones.
set_target_properties(eacp-simd PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)

# Always build the SIMD kernels at the platform maximum optimization, in EVERY
# configuration including Debug: at -O0/-Od the intrinsics spill badly and the
# whole point of this module (fast image hot loops) is lost, while the rest of
# eacp keeps the config default + debug symbols. The optimization flag is
# appended after the config's level so it wins, and -g / /Zi debug info is
# untouched -- Debug builds of this target stay debuggable, just fast.
#
# MSVC's maximum-speed level is /O2 (there is no /O3). In a Debug build the
# default runtime checks (/RTC1) are incompatible with ANY optimization (error
# D8016), so strip them first. This set() shadows the cache variable within the
# SIMD subdirectory only (eacp-simd is its sole target), so the rest of the
# project keeps /RTC1.
if (MSVC)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG
            "${CMAKE_CXX_FLAGS_DEBUG}")
endif ()

# -ffp-contract=off keeps every backend bit-exact with the scalar oracle: no
# silent multiply-add fusion (FMA), which would shift the low bit. Maximum
# optimization does NOT enable fast-math, so IEEE FP semantics (and the
# bit-exact cross-backend guarantee) are preserved. MSVC does not contract by
# default (/fp:precise), so the contraction flag is only needed for Clang/GCC.
target_compile_options(eacp-simd PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O3>
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
        $<$<CXX_COMPILER_ID:MSVC>:/O2>)

eacp_enable_unity_build(eacp-simd)
add_ide_sources(eacp-simd)
