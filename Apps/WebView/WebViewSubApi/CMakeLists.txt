eacp_add_webview_app(WebViewSubApi
        APP_HEADER  App.h
        SOURCES     Main.cpp
        # Codegen runs against the INNER api, so the TS client is written with
        # `greet` / `ticks` at the ROOT — exactly what a client generated for a
        # standalone product looks like. The app then mounts that api NESTED at
        # runtime (App.h binds Api::RootApi), so the wire names gain a prefix the
        # client knows nothing about. That mismatch is the thing under test.
        API         Api::GreeterApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    SubApiApp
        BUNDLE_ID   com.eacp.webviewsubapi
        BUNDLE_NAME "WebView Sub-API")

add_subdirectory(Tests)
