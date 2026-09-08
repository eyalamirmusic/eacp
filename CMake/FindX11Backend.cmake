include(FindPkgConfig)

# The X11 half of the Linux graphics backend: xcb and the extension libraries
# a window needs - XKB for the keymap, RandR for the outputs, XFixes for the
# hidden cursor a mouse lock wants, xcb-cursor for the themed ones, and
# xcb-icccm for the window-manager hints. No Xlib anywhere (plan.md D2).

if (NOT TARGET eacp-x11)
    pkg_check_modules(EACP_X11 IMPORTED_TARGET
            xcb
            xcb-xkb
            xkbcommon-x11
            xcb-randr
            xcb-xfixes
            xcb-cursor
            xcb-icccm)

    if (NOT EACP_X11_FOUND)
        message(FATAL_ERROR
                "A Linux build needs the xcb client libraries, and they were "
                "not found. On Debian/Ubuntu:\n"
                "  sudo apt-get install libxcb1-dev libxcb-xkb1-dev "
                "libxkbcommon-x11-dev libxcb-randr0-dev libxcb-xfixes0-dev "
                "libxcb-cursor-dev libxcb-icccm4-dev pkg-config")
    endif ()

    add_library(eacp-x11 INTERFACE)

    target_link_libraries(eacp-x11 INTERFACE PkgConfig::EACP_X11)
endif ()
