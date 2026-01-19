add_executable(GUI Main.cpp)
target_link_libraries(GUI PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.guiapp")

set_target_properties(GUI PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "GUI App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(GUI)