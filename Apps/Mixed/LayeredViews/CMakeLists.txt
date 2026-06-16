add_executable(LayeredViews Main.cpp)
target_link_libraries(LayeredViews PRIVATE eacp-gpu eacp-webview)

set(BUNDLE_ID "com.eacp.layeredviews")

set_target_properties(LayeredViews PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Layered Views"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(LayeredViews)

# Opt into the dev affordances (build-config only, no app code): harness
# launcher, ASan, and the window debug/capture server — so an agent can
# screenshot / record / drive the composited window (GPU + vector + WebView
# layers at once) over MCP with zero changes to Main.cpp.
eacp_enable_dev_affordances(LayeredViews)
