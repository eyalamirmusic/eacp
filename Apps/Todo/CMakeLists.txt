add_executable(Todo Main.cpp)
target_link_libraries(Todo PRIVATE eacp-graphics)

set_target_properties(Todo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Todo App"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.eacp.todoapp")

set_default_target_setting(Todo)
