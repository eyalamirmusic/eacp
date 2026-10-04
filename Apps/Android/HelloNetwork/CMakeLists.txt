eacp_add_app(HelloNetwork Main.cpp
        DISPLAY_NAME "Hello Network")
target_link_libraries(HelloNetwork PRIVATE eacp-gpu eacp-network)
set_default_target_setting(HelloNetwork)
