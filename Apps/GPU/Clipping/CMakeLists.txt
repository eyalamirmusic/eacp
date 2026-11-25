eacp_add_app(Clipping Main.cpp
        DESCRIPTION "Two scrolling panes clipped by scissor rects. Scroll over either.")
target_link_libraries(Clipping PRIVATE eacp-sprites)
set_target_properties(Clipping PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Clipping)
set_default_target_setting(Clipping)
