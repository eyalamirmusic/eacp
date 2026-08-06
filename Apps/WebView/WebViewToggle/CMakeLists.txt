eacp_add_webview_app(WebViewToggle
        APP_HEADER  App.h
        SOURCES     Main.cpp
        API         Api::PanelApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    ToggleApp
        BUNDLE_ID   com.eacp.webviewtoggle
        BUNDLE_NAME "WebView Toggle"
        REACT)

add_subdirectory(Tests)
