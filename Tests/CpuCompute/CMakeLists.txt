# The CPU executor's own suite: device-free, so it runs on every lane, a Linux
# box with no driver included.
nano_add_executable(CpuComputeTests
        SOURCES BatchTests.cpp DispatchGroupsTests.cpp ExecutorTests.cpp
                GroupTests.cpp HelperTests.cpp KernelTests.cpp SimdMatrixTests.cpp
        TARGETS eacp-cpu-compute)

# The executor never contracts a multiply-add, so the inline C++ references the
# cases compare against must not either, or an exact comparison fails on a
# fused rounding. Clang contracts by default even at -O0; cl never does
# (/fp:precise), and clang-cl only hears the flag through /clang:.
if (MSVC)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(CpuComputeTests PRIVATE /clang:-ffp-contract=off)
    endif ()
else ()
    target_compile_options(CpuComputeTests PRIVATE -ffp-contract=off)
endif ()

# The interpreter against hand-written loops. The stream kernels need nothing
# but the executor; BinKernel is eacp-gpuwidgets', so that half is built only
# where the widgets are.
add_executable(CpuComputeBench CpuComputeBench.cpp)
target_link_libraries(CpuComputeBench PRIVATE eacp-cpu-compute)

if (TARGET eacp-gpuwidgets)
    target_link_libraries(CpuComputeBench PRIVATE eacp-gpuwidgets)
    target_compile_definitions(CpuComputeBench PRIVATE EACP_BENCH_PATHS=1)
endif ()

# Optimised as SimdBench is, and only outside Debug for the same reason: the
# directory-scoped /RTC1 strip would otherwise reach CpuComputeTests. The loops
# it times the executor against take the executor's own flags - no contraction
# (which eacp_force_optimization adds outside Debug, and which is repeated here
# so a Debug build still compares exactly) and no errno.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    eacp_force_optimization(CpuComputeBench)
endif ()

if (MSVC)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(CpuComputeBench PRIVATE
                /clang:-ffp-contract=off /clang:-fno-math-errno)
    endif ()
else ()
    target_compile_options(CpuComputeBench PRIVATE
            -ffp-contract=off -fno-math-errno)
endif ()

eacp_skip_pch(CpuComputeBench)
