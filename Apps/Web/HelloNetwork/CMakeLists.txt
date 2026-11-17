# Its node and headless Chrome runs are ctest entries too (Tests/CMakeLists.txt).
eacp_add_app(HelloNetwork Main.cpp
        DISPLAY_NAME "Hello Network"
        DESCRIPTION "The network client under the browser's rules: CORS, headers, redirects."
        COMPANION "${PROJECT_SOURCE_DIR}/Tests/Network/Web/TestServer.mjs")
target_link_libraries(HelloNetwork PRIVATE eacp-network)
set_default_target_setting(HelloNetwork)
