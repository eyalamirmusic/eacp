add_library(eacp-graphics STATIC)

target_sources(eacp-graphics PRIVATE
        Helpers/DisplayLink.cpp
        Image/Image.cpp
        Image/ImageOps.cpp
        Layers/Layer.cpp
        Layers/LayerViews.cpp
        Layers/ShapeLayer.cpp
        Layers/TextLayer.cpp
        Menu/Menu.cpp
        Primitives/Primitives.cpp
        View/View.cpp
        Widgets/TextInput.cpp)

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
            Primitives/Path-Windows.cpp
            Primitives/TextMetrics-Windows.cpp
            Tray/TrayIcon-Windows.cpp
            View/View-Windows.cpp
            Window/CompositionHostWindow-Windows.cpp
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
            windowscodecs
            windowsapp
            runtimeobject)
elseif (UNIX)
    target_sources(eacp-graphics PRIVATE
            HotKey/GlobalHotKey-Linux.cpp)
endif ()

set_default_target_setting(eacp-graphics)
eacp_enable_unity_build(eacp-graphics)
add_ide_sources(eacp-graphics)
