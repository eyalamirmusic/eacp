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
        Codegen/SpirvEmitter.cpp
        Codegen/ShaderBuilder.cpp)

if (EACP_GPU_BACKEND STREQUAL "Vulkan")
    set(EACP_MOLTENVK_XCFRAMEWORK "" CACHE PATH
            "MoltenVK.xcframework carrying iOS slices; required for iOS Vulkan builds")

    # Every source is portable C++ against the loader: nothing in the Vulkan
    # backend touches Metal, Cocoa or Win32. Off-screen rendering hands back a
    # Graphics::Image, which is the same seam the native backends use for
    # View::renderToImage, so the platform's own 2D layer composites it unchanged.
    target_sources(eacp-gpu PRIVATE
            Buffer/Buffer-Vulkan.cpp
            Codegen/ShaderBuilder-Vulkan.cpp
            CommandBuffer/CommandBuffer-Vulkan.cpp
            Device/Device-Vulkan.cpp
            Frame/ComputePass-Vulkan.cpp
            Frame/Frame-Vulkan.cpp
            Frame/RenderPass-Vulkan.cpp
            Pipeline/ComputePipeline-Vulkan.cpp
            Pipeline/RenderPipeline-Vulkan.cpp
            Shader/ShaderLibrary-Vulkan.cpp
            Texture/Texture-Vulkan.cpp
            View/GPUView-Vulkan.cpp
            Vulkan/VulkanContext-Vulkan.cpp)

    # iOS has no loader, so find_package(Vulkan) cannot succeed there: it requires
    # a library named "vulkan", and MoltenVK is the implementation itself.
    if (IOS)
        if (NOT EACP_MOLTENVK_XCFRAMEWORK)
            message(FATAL_ERROR
                    "iOS Vulkan builds need -DEACP_MOLTENVK_XCFRAMEWORK=<path to a "
                    "MoltenVK.xcframework carrying iOS slices>. Homebrew's molten-vk "
                    "ships a macOS slice only; the LunarG SDK ships iOS ones.")
        endif ()

        # Headers are platform-independent, so a host copy serves the iOS build.
        find_path(EACP_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h REQUIRED
                NO_CMAKE_FIND_ROOT_PATH)

        target_include_directories(eacp-gpu SYSTEM PRIVATE
                ${EACP_VULKAN_INCLUDE_DIR})
        target_link_libraries(eacp-gpu PRIVATE "${EACP_MOLTENVK_XCFRAMEWORK}")
    else ()
        find_package(Vulkan REQUIRED)
        target_link_libraries(eacp-gpu PRIVATE Vulkan::Vulkan)
    endif ()

    # Presentation and zero-copy frame import are the parts that cannot be
    # portable, behind the GPUViewSurface-Vulkan.h and TextureImport-Vulkan.h seams.
    if (APPLE)
        target_sources(eacp-gpu PRIVATE
                Texture/TextureImport-VulkanApple.mm
                View/GPUView-VulkanApple.mm)
        target_link_libraries(eacp-gpu PRIVATE
                "-framework CoreVideo"
                "-framework Foundation"
                "-framework Metal"
                "-framework QuartzCore")

        # Only the backing-scale lookup differs, as in the Metal branch below.
        if (IOS)
            # MoltenVK is linked in rather than loaded, so its own dependencies
            # land here.
            target_sources(eacp-gpu PRIVATE View/GPUView-iOS.mm)
            target_link_libraries(eacp-gpu PRIVATE
                    "-framework IOSurface"
                    "-framework UIKit")
        else ()
            target_sources(eacp-gpu PRIVATE View/GPUView-macOS.mm)
            target_link_libraries(eacp-gpu PRIVATE "-framework Cocoa")
        endif ()
    endif ()
elseif (APPLE)
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
