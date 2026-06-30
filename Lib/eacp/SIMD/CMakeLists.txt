add_library(eacp-simd STATIC)
target_include_directories(eacp-simd PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

target_sources(eacp-simd PRIVATE
        Dispatch/Cpu.cpp
        Dispatch/Dispatch.cpp
        Tu/ArrayOps.cpp
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

# Build the SIMD kernels at the platform maximum optimization in EVERY config
# (including Debug): at -O0/-Od the intrinsics spill badly and the whole point of
# this module -- fast image hot loops -- is lost. The helper keeps results
# bit-exact with the scalar oracle (no fast-math, no FMA contraction), leaves
# debug info intact, and handles the cl / clang-cl / GCC driver differences.
eacp_force_optimization(eacp-simd)

eacp_enable_unity_build(eacp-simd)
add_ide_sources(eacp-simd)
