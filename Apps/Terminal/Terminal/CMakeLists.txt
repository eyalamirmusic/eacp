add_executable(Terminal
        Main.cpp
        TermScreen.cpp
        TermParser.cpp
        TerminalView.cpp)

# Portable core (parser, screen, view) + per-platform PTY and glyph raster.
if (APPLE)
    target_sources(Terminal PRIVATE
            Pty-Posix.cpp
            GlyphAtlas-macOS.mm)
endif ()

target_link_libraries(Terminal PRIVATE eacp-sprites)

set(BUNDLE_ID "com.eacp.terminal")

set_target_properties(Terminal PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Terminal"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Terminal)
set_default_target_setting(Terminal)
