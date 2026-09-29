# Glyph rasterization and atlas caching. The atlas, packer, upload path and the
# UAX #9 bidi pass are portable C++ over eacp-gpu; only the rasterizer is
# per-platform (CoreText on Apple, DirectWrite on Windows, android.graphics
# over JNI on Android, Canvas2D on the web, FreeType + HarfBuzz + fontconfig on
# Linux), which is
# what lets the rest be tested against a stub glyph source instead of against
# installed fonts. Bidi.cpp builds everywhere and is called only by the Linux
# rasterizer: CoreText and DirectWrite reorder inside themselves, and the
# Android and web ones shape a code point at a time.
add_library(eacp-text STATIC
        Bidi.cpp
        GlyphAtlas.cpp
        GlyphRenderer.cpp
        ShelfPacker.cpp
        TextRenderer.cpp)

if (APPLE)
    target_sources(eacp-text PRIVATE GlyphRasterizer-Apple.mm)
    target_link_libraries(eacp-text PRIVATE "-framework CoreText" "-framework CoreGraphics")
elseif (WIN32)
    target_sources(eacp-text PRIVATE GlyphRasterizer-Windows.cpp)
    target_link_libraries(eacp-text PRIVATE dwrite)
elseif (ANDROID)
    target_sources(eacp-text PRIVATE GlyphRasterizer-Android.cpp)
    target_link_libraries(eacp-text PRIVATE jnigraphics)
elseif (EMSCRIPTEN)
    # measureText and fillText on an OffscreenCanvas, read back through
    # getImageData.
    target_sources(eacp-text PRIVATE GlyphRasterizer-Web.cpp)
elseif (UNIX)
    include(FindLinuxText)

    target_sources(eacp-text PRIVATE GlyphRasterizer-Linux.cpp)

    # PRIVATE, so no FreeType, HarfBuzz or fontconfig header reaches a consumer.
    target_link_libraries(eacp-text PRIVATE eacp-linux-text)
endif ()

target_link_libraries(eacp-text PUBLIC eacp-gpu)

set_default_target_setting(eacp-text)
eacp_enable_unity_build(eacp-text)
add_ide_sources(eacp-text)
