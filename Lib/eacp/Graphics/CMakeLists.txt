add_library(eacp-graphics STATIC)

set_default_target_setting(eacp-graphics)
add_ide_sources(eacp-graphics)

target_sources(eacp-graphics PRIVATE Graphics.cpp)

if (APPLE)
    target_sources(eacp-graphics PRIVATE Graphics.mm)
    target_link_libraries(eacp-graphics PUBLIC eacp-core PRIVATE
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework WebKit")

    if (EACP_IOS)
        target_sources(eacp-graphics PRIVATE Graphics-iOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework UIKit"
                "-framework CoreVideo")
    else ()
        target_sources(eacp-graphics PRIVATE Graphics-MacOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework Cocoa"
                "-framework CoreVideo"
                "-framework Carbon")
    endif ()
elseif (WIN32)
    target_sources(eacp-graphics PRIVATE Graphics-Windows.cpp)

    find_package(WebView2 REQUIRED)

    target_link_libraries(eacp-graphics PUBLIC eacp-core PRIVATE
            d2d1
            dwrite
            d3d11
            dxgi
            user32
            gdi32
            ole32
            windowsapp
            runtimeobject
            WebView2::WebView2)
endif ()
