add_executable(PathStroke Main.cpp)
target_link_libraries(PathStroke PRIVATE eacp-gpuwidgets)

set(BUNDLE_ID "com.eacp.pathstroke")

set_target_properties(PathStroke PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "PathStroke"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(PathStroke)
