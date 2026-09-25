# The CPU executor's own suite: device-free, so it runs on every lane, a Linux
# box with no driver included.
nano_add_executable(CpuComputeTests
        SOURCES ExecutorTests.cpp GroupTests.cpp HelperTests.cpp KernelTests.cpp
                SimdMatrixTests.cpp
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
