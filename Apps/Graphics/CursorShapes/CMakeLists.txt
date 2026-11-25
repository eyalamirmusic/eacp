eacp_add_app(CursorShapes Main.cpp
        BUNDLE_ID "com.eacp.cursorshapes"
        DISPLAY_NAME "Cursor Shapes")
target_link_libraries(CursorShapes PRIVATE eacp-ui)
set_target_properties(CursorShapes PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(CursorShapes)
set_default_target_setting(CursorShapes)
