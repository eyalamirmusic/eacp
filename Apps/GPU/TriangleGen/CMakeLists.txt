eacp_add_app(TriangleGen Main.cpp
        DESCRIPTION "The triangle again, its shader and vertex layout generated from C++.")
target_link_libraries(TriangleGen PRIVATE eacp-gpu)
set_target_properties(TriangleGen PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(TriangleGen)
set_default_target_setting(TriangleGen)
