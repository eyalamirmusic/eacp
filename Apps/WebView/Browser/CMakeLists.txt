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
eacp_set_app_icon(Browser
        ICNS "${CMAKE_CURRENT_SOURCE_DIR}/Icon.icns"
        ICO "${CMAKE_CURRENT_SOURCE_DIR}/Icon.ico")

# The Alt-Tab override travels as a plain embedded resource; Main.cpp
# decodes it into WindowOptions::altTabIcon. The application icon itself is
# baked in by eacp_set_app_icon above — no runtime copy needed.
res_embed_add(Browser
        FILES     AltTabIcon.png
        NAMESPACE Browser
        CATEGORY  Browser)
