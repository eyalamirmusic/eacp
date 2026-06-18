add_executable(Compute Main.cpp)
target_link_libraries(Compute PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.compute")

set_target_properties(Compute PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Compute"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(Compute)
