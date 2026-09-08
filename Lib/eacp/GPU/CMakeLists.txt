add_subdirectory(Spirv)

# The shader codegen layer is pure portable C++ (string generation only), so it
# is its own target and builds on every platform. eacp-gpu links it publicly.
add_library(eacp-gpu-codegen STATIC)

target_link_libraries(eacp-gpu-codegen PUBLIC eacp-core)

# The EACP_SHADER reflection macro engine uses __VA_OPT__, which MSVC only
# supports under its conforming preprocessor. The macro lives in a public header,
# so the flag is PUBLIC to reach consumers. Mirrors Miro's MIRO_REFLECT setup.
target_compile_options(eacp-gpu-codegen PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

target_sources(eacp-gpu-codegen PRIVATE
        Codegen/PackedVertex.cpp
        Codegen/ShaderBuilder.cpp
        Codegen/ShaderEmitter.cpp
        Codegen/ShaderGraph.cpp)

# Which dialect build() calls the native one: selection only, no backend.
if (APPLE)
    target_sources(eacp-gpu-codegen PRIVATE Codegen/ShaderBuilder-Apple.cpp)
elseif (WIN32)
    target_sources(eacp-gpu-codegen PRIVATE Codegen/ShaderBuilder-Windows.cpp)
else ()
    target_sources(eacp-gpu-codegen PRIVATE Codegen/ShaderBuilder-Linux.cpp)
endif ()

set_default_target_setting(eacp-gpu-codegen)
eacp_enable_unity_build(eacp-gpu-codegen)
add_ide_sources(eacp-gpu-codegen)

if (NOT EACP_HAS_GPU)
    return ()
endif ()

add_library(eacp-gpu STATIC)

target_link_libraries(eacp-gpu PUBLIC eacp-graphics eacp-gpu-codegen)

target_sources(eacp-gpu PRIVATE
        Buffer/StreamingBuffers.cpp
        Device/Device.cpp
        Frame/RenderPass.cpp
        Timing/FrameTimer.cpp
        Texture/MipChain.cpp)

if (APPLE)
    # The Metal backend is shared across macOS and iOS (the -Apple files); only
    # GPUView's backing-scale lookup is platform-specific.
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Apple.mm
            CommandBuffer/CommandBuffer-Apple.mm
            Device/Device-Apple.mm
            Frame/ComputePass-Apple.mm
            Frame/Frame-Apple.mm
            Frame/RenderPass-Apple.mm
            Pipeline/ComputePipeline-Apple.mm
            Pipeline/RenderPipeline-Apple.mm
            Shader/ShaderLibrary-Apple.mm
            Texture/Texture-Apple.mm
            Timing/GpuTimestamps-Apple.mm
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
            CommandBuffer/CommandBuffer-Windows.cpp
            Device/Device-Windows.cpp
            Frame/ComputePass-Windows.cpp
            Frame/Frame-Windows.cpp
            Frame/RenderPass-Windows.cpp
            Pipeline/ComputePipeline-Windows.cpp
            Pipeline/RenderPipeline-Windows.cpp
            Shader/ShaderLibrary-Windows.cpp
            Texture/Texture-Windows.cpp
            Timing/GpuTimestamps-Windows.cpp
            View/GPUView-Windows.cpp
            Windows/D3D12Context-Windows.cpp)
    target_link_libraries(eacp-gpu PRIVATE
            d3d12
            dxgi
            d3dcompiler
            windowsapp
            runtimeobject)
else ()
    # The Vulkan backend. Nothing here links or includes libwayland: the
    # wl_display and wl_surface it presents to are opaque pointers.
    if (NOT TARGET eacp-spirv)
        message(FATAL_ERROR
                "A Linux build needs the SPIR-V compiler: the Vulkan "
                "backend has no shader compiler in the OS and builds every "
                "pipeline from GLSL through glslang. Configure with "
                "-DEACP_BUILD_SPIRV=ON.")
    endif ()

    find_package(VulkanBackend REQUIRED)

    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Linux.cpp
            CommandBuffer/CommandBuffer-Linux.cpp
            Device/Device-Linux.cpp
            Frame/ComputePass-Linux.cpp
            Frame/Frame-Linux.cpp
            Frame/RenderPass-Linux.cpp
            Pipeline/ComputePipeline-Linux.cpp
            Pipeline/RenderPipeline-Linux.cpp
            Shader/ShaderLibrary-Linux.cpp
            Texture/Texture-Linux.cpp
            Timing/GpuTimestamps-Linux.cpp
            View/GPUView-Linux.cpp
            Vulkan/VulkanContext-Linux.cpp)

    # Both PRIVATE: neither Vulkan nor glslang reaches past this library.
    target_link_libraries(eacp-gpu PRIVATE eacp-vulkan eacp-spirv)
endif ()

set_default_target_setting(eacp-gpu)
eacp_enable_unity_build(eacp-gpu)
add_ide_sources(eacp-gpu)
