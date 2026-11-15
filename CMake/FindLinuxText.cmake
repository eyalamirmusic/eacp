find_package(PkgConfig ${EACP_FIND_QUIET} REQUIRED)

# The Linux text stack: FreeType rasterizes the outlines, HarfBuzz shapes the
# runs, fontconfig resolves families and codepoints. harfbuzz-raster (HarfBuzz
# 13+) is optional: where it is found it paints the COLRv1 colour glyphs
# FreeType leaves as outlines.

option(EACP_USE_HB_RASTER "Paint COLRv1 colour glyphs through harfbuzz-raster" ON)

if (NOT TARGET eacp-linux-text)
    pkg_check_modules(EACP_LINUX_TEXT ${EACP_FIND_QUIET} IMPORTED_TARGET
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

    if (EACP_USE_HB_RASTER)
        pkg_check_modules(EACP_HB_RASTER ${EACP_FIND_QUIET} IMPORTED_TARGET
                harfbuzz-raster)
    endif ()

    if (EACP_USE_HB_RASTER AND EACP_HB_RASTER_FOUND)
        target_link_libraries(eacp-linux-text INTERFACE PkgConfig::EACP_HB_RASTER)
        target_compile_definitions(eacp-linux-text INTERFACE EACP_HAS_HB_RASTER=1)
    endif ()
endif ()
