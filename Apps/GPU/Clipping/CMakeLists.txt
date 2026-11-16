eacp_add_app(Clipping Main.cpp)
target_link_libraries(Clipping PRIVATE eacp-sprites)
set_target_properties(Clipping PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Clipping)
set_default_target_setting(Clipping)

if (EMSCRIPTEN)
    eacp_web_executable(Clipping WEBGPU)
endif ()
