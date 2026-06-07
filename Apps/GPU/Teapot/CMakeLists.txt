add_executable(Teapot Main.cpp)
target_link_libraries(Teapot PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.teapot")

set_target_properties(Teapot PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Teapot"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(Teapot)
