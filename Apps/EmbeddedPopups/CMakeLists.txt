add_executable(EmbeddedPopups Main.cpp)
target_link_libraries(EmbeddedPopups PRIVATE eacp-webview)

eacp_webview_add_vite(EmbeddedPopups
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
        NAMESPACE WebResources
        CATEGORY EmbeddedPopupsApp)

set(BUNDLE_ID "com.eacp.embeddedpopups")

set_target_properties(EmbeddedPopups PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Embedded Popups"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(EmbeddedPopups)
