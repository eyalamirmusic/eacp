eacp_add_app(KeyInspector Main.cpp
        BUNDLE_ID "com.eacp.keyinspector"
        DISPLAY_NAME "Key Inspector")
target_link_libraries(KeyInspector PRIVATE eacp-ui)
set_target_properties(KeyInspector PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(KeyInspector)
set_default_target_setting(KeyInspector)
