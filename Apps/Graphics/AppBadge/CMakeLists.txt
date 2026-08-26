add_executable(AppBadge Main.cpp)
target_link_libraries(AppBadge PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.appbadge")

set_target_properties(AppBadge PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "App Badge"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(AppBadge)
set_default_target_setting(AppBadge)
