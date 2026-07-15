add_executable(VideoRecorderGpu Main.cpp)
target_link_libraries(VideoRecorderGpu PRIVATE eacp-gpu eacp-video)

set(BUNDLE_ID "com.eacp.videorecordergpu")

set_target_properties(VideoRecorderGpu PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "VideoRecorderGpu"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(VideoRecorderGpu)
set_default_target_setting(VideoRecorderGpu)
