add_subdirectory(Spirv)

# The shader codegen layer is pure portable C++ (string generation only): a
# graph built from the EDSL and walked into MSL or HLSL text, with no device, no
# window and no Graphics library under it. It is its own target so it builds on
# every platform, Linux included, and so the emitter can be unit-tested there
# long before a backend exists. eacp-gpu links it publicly, so a consumer of the
# GPU module sees no difference.
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

# Which dialect build() calls the native one. Pure selection - each file is a
# one-line call into an emitter that is compiled everywhere - so it lives with
# the codegen layer rather than with the backend it names.
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
    # The Vulkan backend: compute, render, and the surface. GPUView-Linux.cpp
    # makes a VkSurfaceKHR over the wl_display and wl_surface the window backend
    # reports through Graphics/View/View-Linux.h, and presents through a
    # swapchain; Frame's drawable constructor submits with the acquire and
    # render-finished semaphores and presents where its off-screen one waits.
    # Nothing here links or includes libwayland - the two pointers are opaque,
    # and VK_USE_PLATFORM_WAYLAND_KHR in CMake/FindVulkanBackend.cmake is the
    # whole of what makes them usable.
    if (NOT TARGET eacp-spirv)
        message(FATAL_ERROR
                "EACP_LINUX_GRAPHICS needs the SPIR-V compiler: the Vulkan "
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

    # Both PRIVATE. Vulkan stops at this library - nothing above eacp-gpu names
    # a VkDevice - and glslang reaches only the backend, so an app that links
    # eacp-gpu gets the shader compiler's code and none of its headers.
    target_link_libraries(eacp-gpu PRIVATE eacp-vulkan eacp-spirv)
endif ()

set_default_target_setting(eacp-gpu)
eacp_enable_unity_build(eacp-gpu)
add_ide_sources(eacp-gpu)
