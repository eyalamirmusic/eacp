eacp_add_app(HelloInput Main.cpp
        DISPLAY_NAME "Hello Input"
        DESCRIPTION "A window on the canvas printing the pointer, wheel and key events it gets.")
target_link_libraries(HelloInput PRIVATE eacp-graphics)
set_default_target_setting(HelloInput)
