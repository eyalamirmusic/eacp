add_executable(ComputeImage Main.cpp)
target_link_libraries(ComputeImage PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.computeimage")

set_target_properties(ComputeImage PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ComputeImage"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(ComputeImage)
