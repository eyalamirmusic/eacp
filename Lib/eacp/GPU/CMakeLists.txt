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
            Vulkan/VulkanContext-Vulkan.cpp
            Vulkan/VulkanLoader-Vulkan.cpp)

    # Headers, and no import library on any platform: every entry point is
    # resolved at runtime through Vulkan/VulkanLoader.h, so there is no SDK to
    # install and an app still starts where there is no driver. VK_NO_PROTOTYPES
    # belongs on the target rather than in a header, because it only works if it
    # is set before *every* translation unit reaches vulkan.h.
    find_package(VulkanHeaders REQUIRED)
    target_link_libraries(eacp-gpu PRIVATE Vulkan::Headers)
    target_compile_definitions(eacp-gpu PRIVATE VK_NO_PROTOTYPES)

    # Presentation and zero-copy frame import are the parts that cannot be
    # portable, behind the GPUViewSurface-Vulkan.h and TextureImport-Vulkan.h
    # seams, and so is opening the loader itself (VulkanLoader.h).
    if (APPLE)
        target_sources(eacp-gpu PRIVATE
                Texture/TextureImport-VulkanApple.mm
                View/GPUView-VulkanApple.mm
                Vulkan/VulkanEntryPoint-VulkanPosix.cpp)
        target_link_libraries(eacp-gpu PRIVATE
                "-framework CoreVideo"
                "-framework Foundation"
                "-framework Metal"
                "-framework QuartzCore")

        # Only the backing-scale lookup differs, as in the Metal branch below.
        if (IOS)
            # iOS has no loader to dlopen: MoltenVK is linked into the executable
            # and the entry point is resolved out of the running image, so its
            # own dependencies land here.
            if (NOT EACP_MOLTENVK_XCFRAMEWORK)
                message(FATAL_ERROR
                        "iOS Vulkan builds need -DEACP_MOLTENVK_XCFRAMEWORK=<path "
                        "to a MoltenVK.xcframework carrying iOS slices>. "
                        "Homebrew's molten-vk ships a macOS slice only; the "
                        "LunarG SDK ships iOS ones.")
            endif ()

            target_sources(eacp-gpu PRIVATE View/GPUView-iOS.mm)
            target_link_libraries(eacp-gpu PRIVATE
                    "${EACP_MOLTENVK_XCFRAMEWORK}"
                    "-framework IOSurface"
                    "-framework UIKit")
        else ()
            target_sources(eacp-gpu PRIVATE View/GPUView-macOS.mm)
            target_link_libraries(eacp-gpu PRIVATE "-framework Cocoa")
        endif ()
    elseif (WIN32)
        # Presentation and pixel-buffer import are both off-screen-only stubs for
        # now; the files say why. Everything else -- pipelines, textures, compute
        # and the whole View::renderToImage path -- is the portable code above.
        target_sources(eacp-gpu PRIVATE
                Texture/TextureImport-VulkanWindows.cpp
                View/GPUView-VulkanWindows.cpp
                Vulkan/VulkanEntryPoint-VulkanWindows.cpp)
    else ()
        target_sources(eacp-gpu PRIVATE
                Vulkan/VulkanEntryPoint-VulkanPosix.cpp)
        target_link_libraries(eacp-gpu PRIVATE ${CMAKE_DL_LIBS})
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
