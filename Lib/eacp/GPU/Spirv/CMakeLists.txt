# On by default only where something ships it: the Vulkan backend has no shader
# compiler in the OS, while Metal and D3D12 compile their own dialects.
if (LINUX)
    set(eacp_build_spirv_default ON)
else ()
    set(eacp_build_spirv_default OFF)
endif ()

option(EACP_BUILD_SPIRV
        "Build the GLSL-to-SPIR-V compiler (default on Linux only)"
        ${eacp_build_spirv_default})

if (NOT EACP_BUILD_SPIRV)
    return ()
endif ()

find_package(Glslang REQUIRED)

add_library(eacp-spirv STATIC)

target_sources(eacp-spirv PRIVATE SpirvCompiler.cpp)

# eacp-core, not eacp-gpu: the wrapper is portable C++ over glslang and builds
# on a tree with no graphics backend at all.
target_link_libraries(eacp-spirv PUBLIC eacp-core)

# PRIVATE so glslang stops here.
target_link_libraries(eacp-spirv PRIVATE eacp-glslang)

set_default_target_setting(eacp-spirv)
eacp_enable_unity_build(eacp-spirv)
add_ide_sources(eacp-spirv)
