option(EACP_BUILD_SPIRV "Build the portable GLSL-to-SPIR-V compiler" ON)

if (NOT EACP_BUILD_SPIRV)
    return ()
endif ()

find_package(Glslang REQUIRED)

add_library(eacp-spirv STATIC)

target_sources(eacp-spirv PRIVATE SpirvCompiler.cpp)

# eacp-core rather than eacp-gpu: the wrapper is portable C++ over glslang and
# has to build on Linux, where neither eacp-gpu nor eacp-graphics exists yet.
target_link_libraries(eacp-spirv PUBLIC eacp-core)

# PRIVATE so glslang stops here. Nothing that ships -- eacp-gpu, the apps, the
# other test binaries -- links a shader compiler at this stage.
target_link_libraries(eacp-spirv PRIVATE eacp-glslang)

set_default_target_setting(eacp-spirv)
eacp_enable_unity_build(eacp-spirv)
add_ide_sources(eacp-spirv)
