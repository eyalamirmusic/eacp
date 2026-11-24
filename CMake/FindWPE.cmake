# WPE WebKit, the web engine behind the WebView module's Linux backend: the
# WebKit 2.0 API (wpe-webkit-2.0) and the WPE Platform API (wpe-platform-2.0)
# the backend subclasses to receive the rendered frames in-process. Soft: it
# sets EACP_WPE_FOUND and leaves deciding whether a missing engine is an error
# to the caller. On Debian/Ubuntu the package is libwpewebkit-2.0-dev.

set(EACP_WPE_FOUND OFF)

if (TARGET eacp-wpe)
    set(EACP_WPE_FOUND ON)
    return ()
endif ()

find_package(PkgConfig ${EACP_FIND_QUIET})

if (NOT PKG_CONFIG_FOUND)
    return ()
endif ()

pkg_check_modules(EACP_WPE ${EACP_FIND_QUIET} IMPORTED_TARGET GLOBAL
        wpe-webkit-2.0>=2.50
        wpe-platform-2.0)

if (NOT EACP_WPE_FOUND)
    set(EACP_WPE_FOUND OFF)
    return ()
endif ()

add_library(eacp-wpe INTERFACE)
target_link_libraries(eacp-wpe INTERFACE PkgConfig::EACP_WPE)
set(EACP_WPE_FOUND ON)
