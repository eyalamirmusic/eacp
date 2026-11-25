eacp_add_app(Console Main.cpp
        BUNDLE_ID com.eacp.consoleapp
        DISPLAY_NAME "Console App"
        DESCRIPTION "A Timer that logs four ticks and quits.")
target_link_libraries(Console PRIVATE eacp-core)

if (APPLE)
    set_target_properties(Console PROPERTIES MACOSX_BUNDLE TRUE)
endif ()
