include(FindPkgConfig)

# The Linux text stack: FreeType rasterizes the outlines, HarfBuzz shapes the
# runs, and fontconfig answers "which file is that family" and "which file has
# this codepoint". Together they are what CoreText is on Apple and DirectWrite
# is on Windows.
#
# Found by pkg-config rather than fetched, for the reason FindWayland.cmake
# gives next door: this is the desktop's own font stack. A vendored FreeType
# would rasterize with different hinting from every other application on the
# screen, and a vendored fontconfig would not see the machine's fontconfig
# configuration at all - which is where the substitution rules, the aliases and
# the user's own font directories live.
#
# Included only from the UNIX branch of Lib/eacp/Text/CMakeLists.txt, which is
# reached only when EACP_LINUX_GRAPHICS is on, so a macOS or Windows configure
# never runs any of it. Linked PRIVATE by eacp-text: nothing above the
# rasterizer sees a FreeType, HarfBuzz or fontconfig header.

if (NOT TARGET eacp-linux-text)
    pkg_check_modules(EACP_LINUX_TEXT IMPORTED_TARGET
            freetype2
            harfbuzz
            fontconfig)

    # Checked by hand rather than with REQUIRED so the message can name the
    # packages: pkg_check_modules(REQUIRED) aborts with "None of the required
    # 'freetype2' were found", which is true and useless.
    if (NOT EACP_LINUX_TEXT_FOUND)
        message(FATAL_ERROR
                "EACP_LINUX_GRAPHICS is ON but the text stack was not found. "
                "On Debian/Ubuntu:\n"
                "  sudo apt-get install libfreetype-dev libharfbuzz-dev "
                "libfontconfig-dev pkg-config\n"
                "and a font or two to draw with:\n"
                "  sudo apt-get install fonts-dejavu-core fonts-dejavu-extra "
                "fonts-noto-color-emoji fonts-droid-fallback")
    endif ()

    # An interface target rather than PkgConfig::EACP_LINUX_TEXT directly, so
    # the three libraries travel as one name and consumers read the same way
    # they do for eacp-wayland and eacp-vulkan.
    add_library(eacp-linux-text INTERFACE)

    target_link_libraries(eacp-linux-text INTERFACE PkgConfig::EACP_LINUX_TEXT)
endif ()
