eacp_add_app(GPUComponents Main.cpp Tiles.cpp Panels.cpp)
target_link_libraries(GPUComponents PRIVATE eacp-ui)
set_target_properties(GPUComponents PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(GPUComponents)
set_default_target_setting(GPUComponents)
