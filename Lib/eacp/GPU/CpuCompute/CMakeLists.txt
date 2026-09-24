# The CPU executor for compute kernels: an interpreter over the ShaderGraph a
# kernel records, so it links against the codegen layer alone and builds on
# every platform, a tree with no graphics backend included.
add_library(eacp-cpu-compute STATIC)

target_sources(eacp-cpu-compute PRIVATE
        Builtins.cpp
        Evaluate.cpp
        Executor.cpp
        Plan.cpp
        Statements.cpp
        Workspace.cpp)

target_link_libraries(eacp-cpu-compute PUBLIC eacp-gpu-codegen)

set_default_target_setting(eacp-cpu-compute)

# An interpreter is pointless at -O0, and an audio thread cannot run one there,
# so it is optimised in every configuration with IEEE semantics intact.
eacp_force_optimization(eacp-cpu-compute)

# errno is never read, and without it sqrt and friends inline. cl has no
# equivalent flag and never sets errno from an inlined intrinsic anyway.
if (MSVC)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(eacp-cpu-compute PRIVATE /clang:-fno-math-errno)
    endif ()
else ()
    target_compile_options(eacp-cpu-compute PRIVATE -fno-math-errno)
endif ()

eacp_enable_unity_build(eacp-cpu-compute)
add_ide_sources(eacp-cpu-compute)
