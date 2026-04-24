add_library(eacp-graphics STATIC)

target_sources(eacp-graphics PRIVATE
        Layers/Layer.cpp
        Layers/LayerViews.cpp
        Layers/ShapeLayer.cpp
        Layers/TextLayer.cpp
        Menu/Menu.cpp
        Primitives/Primitives.cpp
        View/View.cpp
        Widgets/TextInput.cpp)

target_link_libraries(eacp-graphics PUBLIC eacp-core)

if (APPLE)
    target_sources(eacp-graphics PRIVATE
            Graphics/GraphicsContextImpl.mm
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
            "-framework CoreVideo")

    if (IOS)
        target_sources(eacp-graphics PRIVATE
                Graphics/Keyboard-iOS.mm
                Helpers/DisplayLink-iOS.mm
                Menu/Menu-iOS.mm
                View/View-iOS.mm
                Window/Window-iOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework UIKit"
                )
    else ()
        target_sources(eacp-graphics PRIVATE
                Graphics/Keyboard-macOS.mm
                Helpers/DisplayLink-macOS.mm
                Menu/Menu-macOS.mm
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
            Helpers/DisplayLink-Windows.cpp
            Layers/Layer-Windows.cpp
            Layers/ShapeLayer-Windows.cpp
            Layers/TextLayer-Windows.cpp
            Menu/Menu-Windows.cpp
            Primitives/Font-Windows.cpp
            Primitives/Path-Windows.cpp
            Primitives/TextMetrics-Windows.cpp
            View/View-Windows.cpp
            Window/EmbeddedView-Windows.cpp
            Window/Window-Windows.cpp)

    target_link_libraries(eacp-graphics PUBLIC eacp-core PRIVATE
            d2d1
            dwrite
            d3d11
            dxgi
            user32
            gdi32
            ole32
            windowsapp
            runtimeobject)
endif ()

set_default_target_setting(eacp-graphics)
eacp_enable_unity_build(eacp-graphics)
add_ide_sources(eacp-graphics)
