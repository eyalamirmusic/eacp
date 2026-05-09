add_executable(WebViewEmbed Main.cpp Commands.cpp)
target_link_libraries(WebViewEmbed PRIVATE eacp-webview eacp-network-rpc)

miro_add_type_export(
        NAME WebViewEmbedSchema
        SOURCES Commands.cpp
        OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
        OUTPUT_NAME schema
        FORMATS ts backend ts-server)

set(WEBVIEWEMBED_CPP_GENERATED_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/cpp-generated)

miro_add_type_export(
        NAME WebViewEmbedCppSchema
        SOURCES Commands.cpp
        OUTPUT_DIR ${WEBVIEWEMBED_CPP_GENERATED_DIR}
        OUTPUT_NAME schema
        FORMATS cpp-miro cpp-client)

# Expose the generated C++ headers to siblings (HttpRpcDemo) without
# forcing them to repeat the codegen.
set(WEBVIEWEMBED_CPP_GENERATED_DIR
        ${WEBVIEWEMBED_CPP_GENERATED_DIR} PARENT_SCOPE)

eacp_webview_generate_backend(
        OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
        BASENAME schema)

add_dependencies(WebViewEmbed WebViewEmbedSchema WebViewEmbedCppSchema)

eacp_webview_add_vite(WebViewEmbed
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
        NAMESPACE WebResources
        CATEGORY WebApp)

set(BUNDLE_ID "com.eacp.webviewembed")

set_target_properties(WebViewEmbed PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Embed"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(WebViewEmbed)
