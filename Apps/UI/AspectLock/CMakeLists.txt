eacp_add_app(AspectLock Main.cpp)
target_link_libraries(AspectLock PRIVATE eacp-ui)
set_target_properties(AspectLock PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(AspectLock)
set_default_target_setting(AspectLock)
