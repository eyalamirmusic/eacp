eacp_add_webview_app(WebViewTodo
        SOURCES           Main.cpp
        TEST_HOST_SOURCES Main-TestHost.cpp
        TESTS_DIR         ${CMAKE_CURRENT_SOURCE_DIR}/tests-node
        API               Api::TodosApi
        API_HEADER        Types.h
        WEB_DIR           ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY          TodoApp
        BUNDLE_ID         com.eacp.webviewtodo
        BUNDLE_NAME       "WebView Todo"
        REACT)
