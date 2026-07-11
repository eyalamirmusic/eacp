eacp_add_webview_app(WebViewReactAnim
        SOURCES     Main.cpp
        API         Api::Clock
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    ReactAnimApp
        BUNDLE_ID   com.eacp.webviewreactanim
        BUNDLE_NAME "WebView React Anim")

# The same app packaged as a runtime-loaded plugin (PLUGIN mode demo). It
# shares the exe's web/ dir and API, so both targets generate identical
# files into web/src/generated; the dependency edges below serialize the
# duplicated generation steps so they never write those files concurrently
# (codegen emits) and the plugin's vite build never runs before schema.ts
# exists (it's emitted by whichever codegen runs first).
eacp_add_webview_app(WebViewReactAnimPlugin PLUGIN
        SOURCES     PluginMain.cpp
        API         Api::Clock
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    ReactAnimApp)

if (TARGET WebViewReactAnimPluginSchema_Codegen)
    add_dependencies(WebViewReactAnimPluginSchema_Codegen
            WebViewReactAnimSchema_Codegen)
endif ()
add_dependencies(WebViewReactAnimPlugin WebViewReactAnim)
