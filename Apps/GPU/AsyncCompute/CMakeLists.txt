add_executable(AsyncCompute Main.cpp)
target_link_libraries(AsyncCompute PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.asynccompute")

set_target_properties(AsyncCompute PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "AsyncCompute"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(AsyncCompute)
