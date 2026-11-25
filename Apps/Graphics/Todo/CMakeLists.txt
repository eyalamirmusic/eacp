eacp_add_app(Todo Main.cpp BUNDLE_ID "com.eacp.todoapp" DISPLAY_NAME "Todo App")
target_link_libraries(Todo PRIVATE eacp-ui)
set_target_properties(Todo PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Todo)
set_default_target_setting(Todo)
