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
            ShapingTests.cpp
            TextRenderingTests.cpp
            TextRendererTests.cpp
            GlyphRendererTests.cpp
            MemoryFontTests.cpp
            VariableFontTests.cpp
            FontPresenceTests.cpp
            EmojiPresentationTests.cpp
            BidiTests.cpp
            TestMain.cpp
        TARGETS eacp-text)

# The end-to-end rendering tests draw through the sprite renderer.
target_link_libraries(TextTests PRIVATE eacp-sprites)

# The shared font registry (memory fonts + PostScript-name resolution) is a
# Windows construction — CoreText gives the Apple side both for free.
if (WIN32)
    target_sources(TextTests PRIVATE FontResolution-WindowsTests.cpp)
endif ()

# Text and off-screen snapshots with no DirectComposition device, the state a
# session with no desktop compositor (a service, a CI runner, session 0 over
# ssh) is in. Its own binary, because its main has to set
# EACP_DCOMP_UNAVAILABLE before the process-wide compositor is first built.
if (WIN32)
    nano_add_executable(NoCompositionTests
            NO_MAIN
            SOURCES
                NoComposition-WindowsTests.cpp
            TARGETS eacp-text)
endif ()

# Which face the emoji property makes the rasterizer choose, and the visual
# order the bidi pass shapes a mixed-direction line in, are the Linux backend's
# own rules; CoreText and DirectWrite decide both for themselves.
if (LINUX)
    target_sources(TextTests PRIVATE
            EmojiPresentation-LinuxTests.cpp
            Bidi-LinuxTests.cpp)
endif ()
