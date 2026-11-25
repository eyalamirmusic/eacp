eacp_add_app(HelloLoop Main.cpp
        DISPLAY_NAME "Hello Loop"
        DESCRIPTION "callAsync, callAfter and a Timer on the page's own loop, then quit.")
target_link_libraries(HelloLoop PRIVATE eacp-core)
set_default_target_setting(HelloLoop)
