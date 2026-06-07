add_library(eacp-gpu STATIC)

target_link_libraries(eacp-gpu PUBLIC eacp-graphics)

# The shader codegen layer is pure portable C++ (string generation only), so it
# builds on every platform and is not gated behind the APPLE/WIN32 branches.
target_sources(eacp-gpu PRIVATE
        Codegen/ShaderGraph.cpp
        Codegen/ShaderEmitter.cpp)

if (APPLE)
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-macOS.mm
            Device/Device-macOS.mm
            Frame/Frame-macOS.mm
            Frame/RenderPass-macOS.mm
            Pipeline/RenderPipeline-macOS.mm
            Shader/ShaderLibrary-macOS.mm
            View/GPUView-macOS.mm)
    target_link_libraries(eacp-gpu PRIVATE
            "-framework Foundation"
            "-framework Metal"
            "-framework QuartzCore")
elseif (WIN32)
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Windows.cpp
            Device/Device-Windows.cpp
            Frame/Frame-Windows.cpp
            Frame/RenderPass-Windows.cpp
            Pipeline/RenderPipeline-Windows.cpp
            Shader/ShaderLibrary-Windows.cpp
            View/GPUView-Windows.cpp)
    target_link_libraries(eacp-gpu PRIVATE
            d3d11
            dxgi
            d3dcompiler
            windowsapp
            runtimeobject)
endif ()

set_default_target_setting(eacp-gpu)
eacp_enable_unity_build(eacp-gpu)
add_ide_sources(eacp-gpu)
