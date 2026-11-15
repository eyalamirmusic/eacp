add_library(fmt STATIC src/format.cc)
target_include_directories(fmt SYSTEM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)

# base.h static_asserts that cl compiles string literals as UTF-8 in every
# translation unit that includes it. PRIVATE, unlike upstream's own CMake: the
# only includer outside this target is eacp-core's Utils/Format.cpp, which sets
# the flag on itself, so nothing else in eacp-core is compiled under it.
if (MSVC)
    target_compile_options(fmt PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif ()

silence_target_warnings(fmt)

# Optimised in every configuration, Debug included, like miniz: formatting at
# -O0 is several times slower, which every Debug log line would pay, and nobody
# steps through fmt. Its own directory keeps the MSVC flag edit this makes
# contained.
eacp_force_optimization(fmt)
