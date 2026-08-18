add_executable(RecordWithAudio Main.cpp)
target_link_libraries(RecordWithAudio PRIVATE eacp-ui eacp-video)

set(BUNDLE_ID "com.eacp.recordwithaudio")

set_target_properties(RecordWithAudio PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "RecordWithAudio"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(RecordWithAudio)
set_default_target_setting(RecordWithAudio)
