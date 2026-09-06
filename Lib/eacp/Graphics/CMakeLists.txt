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

# Portable C++ that nonetheless stands on the 2D drawing stack: the retained
# shape/text layers, the views that wrap one, and the text field drawn out of
# them. Linux has no Context and no Font yet - it has a Path, but only as
# geometry (plan stage 6 decides whether it ever gets a 2D backend or draws
# this tier through the GPU coverage rasterizer instead), so these are left out
# there rather than stubbed.
if (APPLE OR WIN32)
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
    # Wayland, plus honest stubs for everything that needs a codec or a desktop
    # environment. There is no 2D context here, so nothing that rasterizes is in
    # the list — Font, TextMetrics, GraphicUtils, ImageConversion, EmbeddedView
    # and GraphicsContextImpl have no Linux half and no caller that needs one.
    #
    # Path is the exception, because a path is geometry rather than pixels:
    # Path-Linux.cpp records the commands and hands the record out through
    # getHandle(), which is all a tessellator or a later 2D backend needs.
    #
    # Everything still degrades to the headless backend this grew out of: under
    # EACP_HEADLESS, and on a machine whose environment names no compositor,
    # waylandDisplay() is null, a Window is built with no surface, and every
    # query answers exactly as it did before any of this existed.
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
            Window/Display-Linux.cpp
            Window/WaylandDisplay-Linux.cpp
            Window/WaylandInput-Linux.cpp
            Window/Window-Linux.cpp)

    # PRIVATE, which is the whole point of the seam: eacp-wayland carries the
    # generated protocol code and the four system libraries, and nothing above
    # eacp-graphics ever sees a Wayland header. The GPU module reaches the
    # surface through the two forward-declared pointers in View/View-Linux.h.
    include(FindWayland)
    target_link_libraries(eacp-graphics PRIVATE eacp-wayland)
endif ()

set_default_target_setting(eacp-graphics)
eacp_enable_unity_build(eacp-graphics)
add_ide_sources(eacp-graphics)

# Build-time host tool, and the one thing in this directory that is not
# portable-by-construction: it decodes a PNG/JPEG through Image, which is
# implemented per platform. Kept out of the Linux branch until there is a codec
# behind it - eacp_set_app_icon(... IMAGE ...) is the only caller, and it is
# only reachable from an app that draws.
if (APPLE OR WIN32)
    add_subdirectory(IconTool)
endif ()
