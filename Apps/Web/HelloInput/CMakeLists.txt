eacp_add_app(HelloInput Main.cpp
        DISPLAY_NAME "Hello Input")
target_link_libraries(HelloInput PRIVATE eacp-graphics)
set_default_target_setting(HelloInput)
eacp_web_executable(HelloInput)
