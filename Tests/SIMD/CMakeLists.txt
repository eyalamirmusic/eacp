nano_add_executable(SimdTests
        SOURCES SimdTests.cpp
        TARGETS eacp-simd ea_data_structures)

add_executable(SimdBench SimdBench.cpp)
target_link_libraries(SimdBench PRIVATE eacp-simd ea_data_structures)

# A benchmark is only meaningful optimized, so force it -- but only outside
# Debug: a Debug run is never representative, and skipping the call there keeps
# eacp_force_optimization's directory-scoped /RTC1 strip off SimdTests, which
# shares this directory and wants its Debug runtime checks intact.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    eacp_force_optimization(SimdBench)
endif ()

# Neither of these links Miro, so neither inherits the /Zc:preprocessor it
# exports, and the shared PCH image is created under that flag.
eacp_skip_pch(SimdTests)
eacp_skip_pch(SimdBench)
