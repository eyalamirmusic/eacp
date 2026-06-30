nano_add_executable(SimdTests
        SOURCES SimdTests.cpp
        TARGETS eacp-simd ea_data_structures)

add_executable(SimdBench SimdBench.cpp)
target_link_libraries(SimdBench PRIVATE eacp-simd ea_data_structures)
target_compile_options(SimdBench PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O3>)
