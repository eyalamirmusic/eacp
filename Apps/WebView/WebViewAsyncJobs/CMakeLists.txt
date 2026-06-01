eacp_add_webview_app(WebViewAsyncJobs
        APP_HEADER  App.h
        SOURCES     Main.cpp
        API         Api::SearchApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    AsyncJobsApp
        BUNDLE_ID   com.eacp.webviewasyncjobs
        BUNDLE_NAME "WebView Async Jobs")

add_subdirectory(Tests)
