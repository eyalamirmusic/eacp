include(FindPkgConfig)

# The Linux text stack: FreeType rasterizes the outlines, HarfBuzz shapes the
# runs, fontconfig resolves families and codepoints.

if (NOT TARGET eacp-linux-text)
    pkg_check_modules(EACP_LINUX_TEXT IMPORTED_TARGET
            freetype2
            harfbuzz
            fontconfig)

    if (NOT EACP_LINUX_TEXT_FOUND)
        message(FATAL_ERROR
                "A Linux build needs the FreeType/HarfBuzz/fontconfig text "
                "stack, and it was not found. On Debian/Ubuntu:\n"
                "  sudo apt-get install libfreetype-dev libharfbuzz-dev "
                "libfontconfig-dev pkg-config\n"
                "and a font or two to draw with:\n"
                "  sudo apt-get install fonts-dejavu-core fonts-dejavu-extra "
                "fonts-noto-color-emoji fonts-droid-fallback")
    endif ()

    add_library(eacp-linux-text INTERFACE)

    target_link_libraries(eacp-linux-text INTERFACE PkgConfig::EACP_LINUX_TEXT)
endif ()
