# On by default only where something ships it: the Vulkan backend has no shader
# compiler in the OS and builds every pipeline from GLSL through glslang, so a
# Linux build fetches it. Metal and D3D12 compile their own dialects, and the
# GLSL the tests emit is compiled for real on every Linux CI lane - the two
# with no Vulkan device included - so a macOS or Windows build does not pay
# for the fetch and the ~2 MB archive. Opting in there is still allowed, which
# is how the GLSL compile checks can be run locally while editing the emitter.
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

# eacp-core rather than eacp-gpu: the wrapper is portable C++ over glslang and
# builds on a Linux tree with EACP_LINUX_GRAPHICS off, where neither eacp-gpu
# nor eacp-graphics exists.
target_link_libraries(eacp-spirv PUBLIC eacp-core)

# PRIVATE so glslang stops here. Only the Vulkan backend and the test binaries
# link this; nothing that ships on another platform does.
target_link_libraries(eacp-spirv PRIVATE eacp-glslang)

set_default_target_setting(eacp-spirv)
eacp_enable_unity_build(eacp-spirv)
add_ide_sources(eacp-spirv)
