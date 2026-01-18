add_executable(Console Main.cpp)
target_link_libraries(Console PRIVATE eacp-core)

set_target_properties(Console PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Console App"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.eacp.consoleapp")