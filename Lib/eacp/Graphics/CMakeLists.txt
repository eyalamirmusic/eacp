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

# The image kernels (resizeBilinear / warpAffineInverse / mirroredCrop) are
# per-pixel hot loops on the gesture-camera fast path. At -O0 their unoptimized
# codegen runs ~17x slower, enough on its own to drop the live feed to ~8fps.
# Compile just this TU at -O2 even in a Debug build so the kernels stay fast while
# the rest of eacp keeps -O0 + symbols for debugging. The -O2 is appended after
# the config's -O0, so it wins; -g is untouched. Clang/GCC only — MSVC's Debug
# config ships /RTC1, which is a hard error combined with /O2 (D8016), and Windows
# debug-camera perf is not the issue addressed here. Release/RelWithDebInfo are
# already -O2 globally, so the generator expression is Debug-scoped and a no-op
# there. (A per-source option also pulls this file out of the unity blob — fine.)
set_source_files_properties(Image/ImageOps.cpp PROPERTIES COMPILE_OPTIONS
        "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:AppleClang,Clang,GNU>>:-O2>")

target_link_libraries(eacp-graphics PUBLIC eacp-core)

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
endif ()

set_default_target_setting(eacp-graphics)
eacp_enable_unity_build(eacp-graphics)
add_ide_sources(eacp-graphics)
