add_executable(UserAgent Main.cpp)
target_link_libraries(UserAgent PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.useragent")

set_target_properties(UserAgent PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "User Agent"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(UserAgent)
set_default_target_setting(UserAgent)
