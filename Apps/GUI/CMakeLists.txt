add_executable(GUI Main.cpp)
target_link_libraries(GUI PRIVATE eacp-graphics)

set_target_properties(GUI PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "GUI App"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.eacp.guiapp")

set_default_target_setting(GUI)