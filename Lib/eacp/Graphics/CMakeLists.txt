add_library(eacp-graphics STATIC)

target_sources(eacp-graphics PRIVATE
        Helpers/DisplayLink.cpp
        Image/Image.cpp
        Image/ImageOps.cpp
        Layers/Layer.cpp
        Menu/Menu.cpp
        Menu/MenuCommands.cpp
        Primitives/Primitives.cpp
        View/View.cpp)

# Portable C++ that nonetheless stands on the 2D drawing stack, which is the
# tier EACP_HAS_CONTEXT names. Left out where there is none, not stubbed.
if (EACP_HAS_CONTEXT)
    target_sources(eacp-graphics PRIVATE
            Layers/LayerViews.cpp
            Layers/ShapeLayer.cpp
            Layers/TextLayer.cpp
            Widgets/TextInput.cpp)
endif ()

# The image hot loops (resizeBilinear / warpAffineInverse / mirroredCrop) now all
# live in eacp-simd, which is always built -O3 -- so ImageOps.cpp holds only thin
# wrappers and needs no per-source optimization flag here. (The old per-config
# per-source -O2 also broke the Xcode generator, which the iOS build uses.)
target_link_libraries(eacp-graphics PUBLIC eacp-core eacp-simd)

# PUBLIC so the capability is a compile-time fact for every consumer, not just
# a CMake variable: Graphics.h leaves the 2D-tier headers out where there is no
# 2D tier, and a caller that reaches for one gets a compile error rather than a
# link error.
target_compile_definitions(eacp-graphics PUBLIC
        EACP_HAS_CONTEXT=$<BOOL:${EACP_HAS_CONTEXT}>)

if (APPLE)
    target_sources(eacp-graphics PRIVATE
            Graphics/GraphicsContextImpl.mm
            Helpers/SystemAppearance-Apple.mm
            Image/Image-Apple.mm
            Layers/Layer.mm
            Layers/ShapeLayer.mm
            Layers/TextLayer.mm
            Primitives/Font.mm
            Primitives/GraphicUtils.mm
            Primitives/Path.mm
            Primitives/TextMetrics.mm)
    target_link_libraries(eacp-graphics PRIVATE
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework CoreVideo"
            "-framework ImageIO")

    if (IOS)
        target_sources(eacp-graphics PRIVATE
                Graphics/Keyboard-iOS.mm
                Helpers/DisplayLink-iOS.mm
                HotKey/GlobalHotKey-iOS.mm
                Menu/Menu-iOS.mm
                Tray/TrayIcon-iOS.mm
                View/View-iOS.mm
                Window/Display-iOS.mm
                Window/Window-iOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework UIKit"
                )
    else ()
        target_sources(eacp-graphics PRIVATE
                Graphics/Keyboard-macOS.mm
                Helpers/DisplayLink-macOS.mm
                Helpers/ImageConversion-macOS.mm
                HotKey/GlobalHotKey-macOS.mm
                Menu/AppKitMenu.mm
                Menu/Menu-macOS.mm
                Tray/TrayIcon-macOS.mm
                View/View-macOS.mm
                Window/Display-macOS.mm
                Window/EmbeddedView-macOS.mm
                Window/Window-macOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework Cocoa"
                "-framework Carbon")
    endif ()
elseif (WIN32)
    target_sources(eacp-graphics PRIVATE
            Graphics/D2DFactory-Windows.cpp
            Graphics/Keyboard-Windows.cpp
            Helpers/DarkMode-Windows.cpp
            Helpers/DisplayLink-Windows.cpp
            Helpers/ImageConversion-Windows.cpp
            HotKey/GlobalHotKey-Windows.cpp
            Image/Image-Windows.cpp
            Layers/Layer-Windows.cpp
            Layers/ShapeLayer-Windows.cpp
            Layers/TextLayer-Windows.cpp
            Menu/Menu-Windows.cpp
            Primitives/Font-Windows.cpp
            Primitives/FontRegistry-Windows.cpp
            Primitives/Path-Windows.cpp
            Primitives/TextMetrics-Windows.cpp
            Tray/TrayIcon-Windows.cpp
            View/View-Windows.cpp
            Window/CompositionHostWindow-Windows.cpp
            Window/Display-Windows.cpp
            Window/EmbeddedView-Windows.cpp
            Window/Window-Windows.cpp)

    target_link_libraries(eacp-graphics PUBLIC eacp-core PRIVATE
            d2d1
            dwrite
            d3d11
            dxgi
            dcomp
            user32
            gdi32
            ole32
            shell32
            windowscodecs)
elseif (UNIX)
    # Wayland, plus stubs for what needs a codec or a desktop environment.
    # Path is the exception, recording geometry rather than pixels.
    target_sources(eacp-graphics PRIVATE
            Graphics/Keyboard-Linux.cpp
            Helpers/DisplayLink-Linux.cpp
            Helpers/SystemAppearance-Linux.cpp
            HotKey/GlobalHotKey-Linux.cpp
            Image/Image-Linux.cpp
            Layers/Layer-Linux.cpp
            Menu/Menu-Linux.cpp
            Primitives/Path-Linux.cpp
            Tray/TrayIcon-Linux.cpp
            View/View-Linux.cpp
            View/WaylandViewSurface-Linux.cpp
            Window/Display-Linux.cpp
            Window/LinuxInput-Linux.cpp
            Window/LinuxWindowSystem-Linux.cpp
            Window/WaylandClipboard-Linux.cpp
            Window/WaylandDisplay-Linux.cpp
            Window/WaylandInput-Linux.cpp
            Window/Window-Linux.cpp)

    # PRIVATE: nothing above eacp-graphics ever sees a Wayland header. The GPU
    # module reaches the surface through the NativeSurfaceHandle in
    # View/View-Linux.h.
    include(FindWayland)
    target_link_libraries(eacp-graphics PRIVATE eacp-wayland)
endif ()

set_default_target_setting(eacp-graphics)
eacp_enable_unity_build(eacp-graphics)
add_ide_sources(eacp-graphics)

# A build-time host tool that decodes a PNG/JPEG through Image, so it needs the
# platform codecs EACP_HAS_CONTEXT names.
if (EACP_HAS_CONTEXT)
    add_subdirectory(IconTool)
endif ()
