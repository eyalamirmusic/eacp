eacp_add_app(HelloLoop Main.cpp
        DISPLAY_NAME "Hello Loop")
target_link_libraries(HelloLoop PRIVATE eacp-core)
set_default_target_setting(HelloLoop)
eacp_web_executable(HelloLoop)
