include(FindPkgConfig)

# The X11 half of the Linux graphics backend: xcb and the extension libraries
# a window needs - XKB for the keymap, RandR for the outputs, XFixes for the
# hidden cursor a mouse lock wants, xcb-cursor for the themed ones, xcb-icccm
# for the window-manager hints, and XInput 2 for the pointer: smooth scrolling
# and the raw motion a locked pointer is measured by. No Xlib anywhere
# (plan.md D2).

if (NOT TARGET eacp-x11)
    pkg_check_modules(EACP_X11 IMPORTED_TARGET
            xcb
            xcb-xkb
            xkbcommon-x11
            xcb-randr
            xcb-xfixes
            xcb-cursor
            xcb-icccm
            xcb-xinput)

    if (NOT EACP_X11_FOUND)
        message(FATAL_ERROR
                "A Linux build needs the xcb client libraries, and they were "
                "not found. On Debian/Ubuntu:\n"
                "  sudo apt-get install libxcb1-dev libxcb-xkb-dev "
                "libxkbcommon-x11-dev libxcb-randr0-dev libxcb-xfixes0-dev "
                "libxcb-cursor-dev libxcb-icccm4-dev libxcb-xinput-dev "
                "pkg-config")
    endif ()

    add_library(eacp-x11 INTERFACE)

    target_link_libraries(eacp-x11 INTERFACE PkgConfig::EACP_X11)
endif ()
