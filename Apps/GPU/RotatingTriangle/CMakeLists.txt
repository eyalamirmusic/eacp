eacp_add_app(RotatingTriangle Main.cpp RotatingShader.h
        DESCRIPTION "A triangle spun by a uniform, its shader written in the C++ EDSL.")
target_link_libraries(RotatingTriangle PRIVATE eacp-gpu)
set_target_properties(RotatingTriangle PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(RotatingTriangle)
set_default_target_setting(RotatingTriangle)

eacp_add_precompiled_shaders(RotatingTriangle SOURCES Shaders.cpp)
