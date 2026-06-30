nano_add_executable(SimdTests
        SOURCES SimdTests.cpp
        TARGETS eacp-simd ea_data_structures)

# Standalone throughput benchmark (scalar vs SIMD). Not a test -- build and run
# it from a Release build; a Debug build compiles every backend at -O0.
add_executable(SimdBench SimdBench.cpp)
target_link_libraries(SimdBench PRIVATE eacp-simd ea_data_structures)
