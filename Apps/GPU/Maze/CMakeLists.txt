add_executable(Maze Main.cpp)
target_link_libraries(Maze PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.maze")

set_target_properties(Maze PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Maze"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Maze)
set_default_target_setting(Maze)
