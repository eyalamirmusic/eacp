add_executable(CursorShapes Main.cpp)
target_link_libraries(CursorShapes PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.cursorshapes")

set_target_properties(CursorShapes PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Cursor Shapes"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(CursorShapes)
set_default_target_setting(CursorShapes)
