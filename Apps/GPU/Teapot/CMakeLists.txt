add_executable(Teapot Main.cpp)
target_link_libraries(Teapot PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.teapot")

set_target_properties(Teapot PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Teapot"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Teapot)
set_default_target_setting(Teapot)
eacp_set_app_icon(Teapot
        ICNS "${CMAKE_CURRENT_SOURCE_DIR}/Icon.icns"
        ICO "${CMAKE_CURRENT_SOURCE_DIR}/Icon.ico")
