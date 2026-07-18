# The packer and the atlas are portable logic, driven here by a stub glyph
# source rather than by an installed font — no GPU device and no platform text
# stack involved, so these run and mean the same thing everywhere eacp-text
# builds.
nano_add_executable(TextTests
        NO_MAIN
        SOURCES
            ShelfPackerTests.cpp
            GlyphAtlasTests.cpp
            GlyphRasterizerTests.cpp
            TextRenderingTests.cpp
            GlyphRendererTests.cpp
            TestMain.cpp
        TARGETS eacp-text)

# The end-to-end rendering tests draw through the sprite renderer.
target_link_libraries(TextTests PRIVATE eacp-sprites)
