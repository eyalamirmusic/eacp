add_executable(PathQuality Main.cpp)
target_link_libraries(PathQuality PRIVATE eacp-gpuwidgets)

set(BUNDLE_ID "com.eacp.pathquality")

set_target_properties(PathQuality PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "PathQuality"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(PathQuality)
