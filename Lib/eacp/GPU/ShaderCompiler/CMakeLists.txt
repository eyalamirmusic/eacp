# The build-time shader compiler: the library a generated shader host executable
# (CMake/ShaderHostMain.cpp.in) links to turn an app's shader graphs into
# precompiled blobs. Device-free like the codegen it links, so it builds wherever
# that does, a CI lane with no GPU included.
add_library(eacp-shader-compiler STATIC)

target_sources(eacp-shader-compiler PRIVATE
        Metal.cpp
        ShaderCompiler.cpp)

if (WIN32)
    target_sources(eacp-shader-compiler PRIVATE Dxbc-Windows.cpp)
    target_link_libraries(eacp-shader-compiler PRIVATE d3dcompiler)
else ()
    target_sources(eacp-shader-compiler PRIVATE Dxbc-Default.cpp)
endif ()

target_link_libraries(eacp-shader-compiler PUBLIC eacp-gpu-codegen)

if (TARGET eacp-spirv)
    target_sources(eacp-shader-compiler PRIVATE Spirv.cpp)
    target_link_libraries(eacp-shader-compiler PRIVATE eacp-spirv)
else ()
    target_sources(eacp-shader-compiler PRIVATE Spirv-Default.cpp)
endif ()

set_default_target_setting(eacp-shader-compiler)
eacp_enable_unity_build(eacp-shader-compiler)
add_ide_sources(eacp-shader-compiler)
