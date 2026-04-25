add_library(eacp-graphics STATIC)

set_default_target_setting(eacp-graphics)
add_ide_sources(eacp-graphics)

target_sources(eacp-graphics PRIVATE Graphics.cpp)

target_link_libraries(eacp-graphics PUBLIC eacp-core)

if (APPLE)
    target_sources(eacp-graphics PRIVATE Graphics.mm)
    target_link_libraries(eacp-graphics PRIVATE
            "-framework Foundation"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework CoreVideo")

    if (IOS)
        target_sources(eacp-graphics PRIVATE Graphics-iOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework UIKit"
                )
    else ()
        target_sources(eacp-graphics PRIVATE Graphics-MacOS.mm)
        target_link_libraries(eacp-graphics PRIVATE
                "-framework Cocoa"
                "-framework Carbon")
    endif ()
elseif (WIN32)
    target_sources(eacp-graphics PRIVATE Graphics-Windows.cpp)

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
