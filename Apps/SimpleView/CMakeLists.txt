add_executable(SimpleView Main.cpp)
target_link_libraries(SimpleView PRIVATE eacp-graphics)

set_target_properties(SimpleView PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Simple View App"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.eacp.simpleview")

set_default_target_setting(SimpleView)