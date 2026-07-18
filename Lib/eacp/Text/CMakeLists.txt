# Glyph rasterization and atlas caching. The atlas, packer and upload path are
# portable C++ over eacp-gpu; only the rasterizer is per-platform (CoreText on
# Apple, DirectWrite on Windows), which is what lets the rest be tested against
# a stub glyph source instead of against installed fonts.
add_library(eacp-text STATIC
        GlyphAtlas.cpp
        ShelfPacker.cpp)

if (APPLE)
    target_sources(eacp-text PRIVATE GlyphRasterizer-Apple.mm)
    target_link_libraries(eacp-text PRIVATE "-framework CoreText" "-framework CoreGraphics")
elseif (WIN32)
    # Stub for now: the DirectWrite backend is the one piece of this module not
    # yet written, and it reports isValid() == false so the build stays green
    # and the gap is explicit. See the file header for what porting it involves.
    target_sources(eacp-text PRIVATE GlyphRasterizer-Windows.cpp)
endif ()

target_link_libraries(eacp-text PUBLIC eacp-gpu)

set_default_target_setting(eacp-text)
eacp_enable_unity_build(eacp-text)
add_ide_sources(eacp-text)
