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

# The icons travel as plain embedded resources; Main.cpp decodes them into
# WindowOptions::applicationIcon and (Windows) the altTabIcon override.
res_embed_add(Browser
        FILES     Icon.png
                  AltTabIcon.png
        NAMESPACE Browser
        CATEGORY  Browser)
