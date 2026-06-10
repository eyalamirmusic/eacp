add_library(eacp-gpu STATIC)

target_link_libraries(eacp-gpu PUBLIC eacp-graphics)

# The EACP_SHADER reflection macro engine uses __VA_OPT__, which MSVC only
# supports under its conforming preprocessor. The macro lives in a public header,
# so the flag is PUBLIC to reach consumers. Mirrors Miro's MIRO_REFLECT setup.
target_compile_options(eacp-gpu PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# The shader codegen layer is pure portable C++ (string generation only), so it
# builds on every platform and is not gated behind the APPLE/WIN32 branches. Only
# the native backend selection (ShaderBuilder-*) lives in the platform branches.
target_sources(eacp-gpu PRIVATE
        Codegen/ShaderGraph.cpp
        Codegen/ShaderEmitter.cpp
        Codegen/ShaderBuilder.cpp)

if (APPLE)
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-macOS.mm
            Codegen/ShaderBuilder-macOS.cpp
            CommandBuffer/CommandBuffer-macOS.mm
            Device/Device-macOS.mm
            Frame/ComputePass-macOS.mm
            Frame/Frame-macOS.mm
            Frame/RenderPass-macOS.mm
            Pipeline/ComputePipeline-macOS.mm
            Pipeline/RenderPipeline-macOS.mm
            Shader/ShaderLibrary-macOS.mm
            Texture/Texture-macOS.mm
            View/GPUView-macOS.mm)
    target_link_libraries(eacp-gpu PRIVATE
            "-framework Foundation"
            "-framework Metal"
            "-framework QuartzCore")
elseif (WIN32)
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Windows.cpp
            Codegen/ShaderBuilder-Windows.cpp
            CommandBuffer/CommandBuffer-Windows.cpp
            Device/Device-Windows.cpp
            Frame/ComputePass-Windows.cpp
            Frame/Frame-Windows.cpp
            Frame/RenderPass-Windows.cpp
            Pipeline/ComputePipeline-Windows.cpp
            Pipeline/RenderPipeline-Windows.cpp
            Shader/ShaderLibrary-Windows.cpp
            Texture/Texture-Windows.cpp
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
