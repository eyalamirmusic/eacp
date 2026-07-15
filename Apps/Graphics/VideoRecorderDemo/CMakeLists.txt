add_executable(VideoRecorderDemo Main.cpp)
target_link_libraries(VideoRecorderDemo PRIVATE eacp-graphics eacp-video)

set(BUNDLE_ID "com.eacp.videorecorderdemo")

set_target_properties(VideoRecorderDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "VideoRecorderDemo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(VideoRecorderDemo)
set_default_target_setting(VideoRecorderDemo)
