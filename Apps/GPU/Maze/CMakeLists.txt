eacp_add_app(Maze Main.cpp
        DESCRIPTION "A first-person maze walk. W/A/S/D and the arrows, click for mouse look.")
target_link_libraries(Maze PRIVATE eacp-gpu)
set_target_properties(Maze PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Maze)
set_default_target_setting(Maze)
