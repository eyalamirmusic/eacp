# Glyph rasterization and atlas caching. The atlas, packer, upload path and the
# UAX #9 bidi pass are portable C++ over eacp-gpu; only the rasterizer is
# per-platform (CoreText on Apple, DirectWrite on Windows, android.graphics
# over JNI on Android, FreeType + HarfBuzz on Linux and the web, finding faces
# through fontconfig on the one and a bundled-font registry on the other),
# which is what lets the rest be tested against a stub glyph source instead of
# against installed fonts. Bidi.cpp builds everywhere and is called only by the
# FreeType rasterizer: CoreText and DirectWrite reorder inside themselves, and
# the Android one shapes a code point at a time.
add_library(eacp-text STATIC
        Bidi.cpp
        GlyphAtlas.cpp
        GlyphRasterizer.cpp
        GlyphRenderer.cpp
        ShelfPacker.cpp
        TextRenderer.cpp
        UnicodeEmoji.cpp)

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
    option(EACP_WEB_DEFAULT_FONTS
            "Embed DejaVu Sans and DejaVu Sans Mono in every web app linking eacp-text"
            ON)

    target_sources(eacp-text PRIVATE
            GlyphRasterizer-Linux.cpp
            FontDiscovery-Web.cpp)

    target_compile_options(eacp-text PRIVATE --use-port=freetype --use-port=harfbuzz)
    target_link_options(eacp-text INTERFACE --use-port=freetype --use-port=harfbuzz)

    if (EACP_WEB_DEFAULT_FONTS)
        set(dejavu "${PROJECT_SOURCE_DIR}/ThirdParty/DejaVu")

        res_embed_add(eacp-text
                FILES
                    "${dejavu}/DejaVuSans.ttf"
                    "${dejavu}/DejaVuSans-Bold.ttf"
                    "${dejavu}/DejaVuSansMono.ttf"
                    "${dejavu}/DejaVuSansMono-Bold.ttf"
                NAMESPACE EacpTextFonts
                CATEGORY  EacpTextFonts)
    endif ()
elseif (UNIX)
    include(FindLinuxText)

    target_sources(eacp-text PRIVATE
            GlyphRasterizer-Linux.cpp
            FontDiscovery-Linux.cpp)

    # PRIVATE, so no FreeType, HarfBuzz or fontconfig header reaches a consumer.
    target_link_libraries(eacp-text PRIVATE eacp-linux-text)
endif ()

target_link_libraries(eacp-text PUBLIC eacp-gpu)

set_default_target_setting(eacp-text)
eacp_enable_unity_build(eacp-text)
add_ide_sources(eacp-text)
