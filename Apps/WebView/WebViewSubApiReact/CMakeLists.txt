eacp_add_webview_app(WebViewSubApiReact
        APP_HEADER  App.h
        SOURCES     Main.cpp
        # As in WebViewSubApi, codegen runs against the INNER api, so the TS
        # client is written with `getCounter` / `counter` at the ROOT while the
        # app mounts that api NESTED (App.h binds Api::RootApi). REACT is what
        # this fixture adds on top: the generated hooks module is what sets up
        # the client's subscriptions, so the ordering between configureBridge
        # and module evaluation becomes eacp's problem rather than the page's.
        API         Api::CounterApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    SubApiReactApp
        BUNDLE_ID   com.eacp.webviewsubapireact
        BUNDLE_NAME "WebView Sub-API React"
        REACT)

add_subdirectory(Tests)
