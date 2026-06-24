add_executable(Teapot Main.cpp)
target_link_libraries(Teapot PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.teapot")

set_target_properties(Teapot PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Teapot"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(Teapot)

# Opt this example into the dev affordances (build-config only, no app
# code): the agent harness launcher, ASan, and the window debug/capture
# server — so the spinning teapot can be screenshotted / recorded over
# MCP with zero changes to Main.cpp.
eacp_enable_dev_affordances(Teapot)
