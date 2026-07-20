add_executable(Todo Main.cpp)
target_link_libraries(Todo PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.todoapp")

set_target_properties(Todo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Todo App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Todo)
set_default_target_setting(Todo)
