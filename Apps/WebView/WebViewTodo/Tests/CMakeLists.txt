eacp_add_webview_test(WebViewTodoTests
        APP     WebViewTodo
        SOURCES WebViewTodoTests.cpp RemoteDebugTests.cpp)

# The app's STATIC lib already propagates the source dir on its
# include path (so #include "App.h" works), so no extra
# target_include_directories needed.

# RemoteDebugTests drives the MCP debug server directly; the app exe
# gets it automatically via EACP_DEBUG_SERVER, but the test binary
# links ${APP}_app and needs the lib explicitly.
if (TARGET WebViewTodoTests AND TARGET eacp-webview-remote)
    target_link_libraries(WebViewTodoTests PRIVATE eacp-webview-remote)
endif ()
