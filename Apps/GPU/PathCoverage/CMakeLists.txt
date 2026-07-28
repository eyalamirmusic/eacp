add_executable(PathCoverage Main.cpp)
target_link_libraries(PathCoverage PRIVATE eacp-gpuwidgets)

set(BUNDLE_ID "com.eacp.pathcoverage")

set_target_properties(PathCoverage PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "PathCoverage"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(PathCoverage)
