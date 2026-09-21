add_executable(ConnectivityDemo Main.cpp)
target_link_libraries(ConnectivityDemo PRIVATE eacp-graphics eacp-network)

set(BUNDLE_ID "com.eacp.connectivitydemo")

set_target_properties(ConnectivityDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Connectivity Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(ConnectivityDemo)
set_default_target_setting(ConnectivityDemo)
