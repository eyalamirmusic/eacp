add_library(miniz STATIC miniz.c)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(miniz PRIVATE MINIZ_NO_STDIO)

if (MSVC)
    target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

silence_target_warnings(miniz)

# Optimised in every configuration, Debug included: a compressor at -O0 runs
# at a fraction of its speed, which is what a Debug app loading a zipped
# resource would feel, and nobody steps through miniz. Its own directory
# keeps the MSVC flag edit this makes contained.
eacp_force_optimization(miniz)
