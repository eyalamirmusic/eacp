add_executable(Browser Main.cpp)
target_link_libraries(Browser PRIVATE eacp-webview)

# The embedded icon useEmbeddedApplicationIcon stamps onto the window.
if (WIN32)
    target_sources(Browser PRIVATE Browser.rc)
endif ()

set(BUNDLE_ID "com.eacp.browser")

set_target_properties(Browser PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Browser"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Browser)
set_default_target_setting(Browser)
