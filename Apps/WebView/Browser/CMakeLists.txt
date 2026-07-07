add_executable(Browser Main.cpp)
target_link_libraries(Browser PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.browser")

set_target_properties(Browser PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Browser"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Browser)
set_default_target_setting(Browser)
# One PNG drives both icons: eacp_set_app_icon generates the at-rest .icns/.ico
# from it at build time, and res_embed_add embeds the same file so Main.cpp can
# decode it into WindowOptions::altTabIcon (the Alt-Tab override).
eacp_set_app_icon(Browser IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/AltTabIcon.png")

res_embed_add(Browser
        FILES     AltTabIcon.png
        NAMESPACE Browser
        CATEGORY  Browser)
