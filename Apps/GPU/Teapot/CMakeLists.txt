eacp_add_app(Teapot Main.cpp ICON "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png")
target_link_libraries(Teapot PRIVATE eacp-gpu)
set_target_properties(Teapot PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Teapot)
set_default_target_setting(Teapot)
