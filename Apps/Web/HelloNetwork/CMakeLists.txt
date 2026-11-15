eacp_add_app(HelloNetwork Main.cpp
        DISPLAY_NAME "Hello Network")
target_link_libraries(HelloNetwork PRIVATE eacp-network)
set_default_target_setting(HelloNetwork)

# Its node and headless Chrome runs are ctest entries too (Tests/CMakeLists.txt).
eacp_web_executable(HelloNetwork
        COMPANION "${PROJECT_SOURCE_DIR}/Tests/Network/Web/TestServer.mjs")
