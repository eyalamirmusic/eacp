# Local persistence (MRU stamps, session restore). Fetched here rather than
# at the top level because only the Terminal app uses it; emberstore reuses
# the eacp/Miro targets already in this build.
CPMAddPackage(
        NAME emberstore
        GITHUB_REPOSITORY tamber-inc/emberstore
        GIT_TAG main
)

add_executable(Terminal
        Main.cpp
        AppShell.cpp
        Config.cpp
        MruStore.cpp
        Palette.cpp
        Popup.cpp
        Projects.cpp
        Session.cpp
        SessionView.cpp
        TermScreen.cpp
        TermParser.cpp
        TerminalView.cpp
        TrayController.cpp
        DaemonClient.cpp)

# Portable core (parser, screen, sessions, palette) + per-platform PTY,
# glyph raster and notifications.
if (APPLE)
    target_sources(Terminal PRIVATE
            Pty-Posix.cpp
            GlyphAtlas-macOS.mm
            Notifier-macOS.mm)

    target_link_libraries(Terminal PRIVATE "-framework UserNotifications")
elseif (WIN32)
    target_sources(Terminal PRIVATE
            Pty-Windows.cpp
            GlyphAtlas-Windows.cpp
            Notifier-Windows.cpp)

    # The parser and screen sources hold Unicode literals (box drawing,
    # powerline glyphs); MSVC assumes the ANSI codepage without this.
    target_compile_options(Terminal PRIVATE /utf-8)

    target_link_libraries(Terminal PRIVATE
            d2d1
            dwrite
            windowscodecs
            user32
            ole32)
endif ()

target_link_libraries(Terminal PRIVATE eacp-sprites eacp-network emberstore)

# The session daemon rides inside the bundle, next to the app binary, so
# the client can launch it with no install step.
add_dependencies(Terminal TerminalDaemon)
add_custom_command(TARGET Terminal POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TerminalDaemon> $<TARGET_FILE_DIR:Terminal>)

# JetBrains Mono (OFL) rides inside the binary; registered with the platform
# font system at startup, so the default font needs no install step.
res_embed_add(Terminal DIRECTORY Resources)

set(BUNDLE_ID "com.eacp.terminal")

set_target_properties(Terminal PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Terminal"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Terminal)
set_default_target_setting(Terminal)
