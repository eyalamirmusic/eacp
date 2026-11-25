eacp_add_app(TaskBoard TaskBoard.cpp
        BUNDLE_ID "com.eacp.taskboard"
        DISPLAY_NAME "Task Board")
target_link_libraries(TaskBoard PRIVATE eacp-ui)
set_target_properties(TaskBoard PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(TaskBoard)
set_default_target_setting(TaskBoard)
