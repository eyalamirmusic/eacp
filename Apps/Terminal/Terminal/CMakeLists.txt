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
        Projects.cpp
        Session.cpp
        SessionView.cpp
        TermScreen.cpp
        TermParser.cpp
        TerminalView.cpp
        TrayController.cpp)

# Portable core (parser, screen, sessions, palette) + per-platform PTY,
# glyph raster and notifications.
if (APPLE)
    target_sources(Terminal PRIVATE
            Pty-Posix.cpp
            GlyphAtlas-macOS.mm
            Notifier-macOS.mm)

    target_link_libraries(Terminal PRIVATE "-framework UserNotifications")
endif ()

target_link_libraries(Terminal PRIVATE eacp-sprites emberstore)

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
