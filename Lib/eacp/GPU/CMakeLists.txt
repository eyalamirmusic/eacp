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
        Device/Device.cpp
        Codegen/ShaderGraph.cpp
        Codegen/ShaderEmitter.cpp
        Codegen/ShaderBuilder.cpp)

if (APPLE)
    # The Metal backend is shared across macOS and iOS (the -Apple files); only
    # GPUView's backing-scale lookup is platform-specific.
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Apple.mm
            Codegen/ShaderBuilder-Apple.cpp
            CommandBuffer/CommandBuffer-Apple.mm
            Device/Device-Apple.mm
            Frame/ComputePass-Apple.mm
            Frame/Frame-Apple.mm
            Frame/RenderPass-Apple.mm
            Pipeline/ComputePipeline-Apple.mm
            Pipeline/RenderPipeline-Apple.mm
            Shader/ShaderLibrary-Apple.mm
            Texture/Texture-Apple.mm
            View/GPUView-Apple.mm)
    target_link_libraries(eacp-gpu PRIVATE
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework Metal"
            "-framework QuartzCore")

    if (IOS)
        target_sources(eacp-gpu PRIVATE View/GPUView-iOS.mm)
        target_link_libraries(eacp-gpu PRIVATE "-framework UIKit")
    else ()
        target_sources(eacp-gpu PRIVATE View/GPUView-macOS.mm)
        target_link_libraries(eacp-gpu PRIVATE "-framework Cocoa")
    endif ()
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
            View/GPUView-Windows.cpp
            Windows/D3D12Context-Windows.cpp)
    target_link_libraries(eacp-gpu PRIVATE
            d3d12
            dxgi
            d3dcompiler
            windowsapp
            runtimeobject)
endif ()

set_default_target_setting(eacp-gpu)
eacp_enable_unity_build(eacp-gpu)
add_ide_sources(eacp-gpu)
