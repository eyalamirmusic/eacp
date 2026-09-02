add_executable(StreamingStress Main.cpp)
target_link_libraries(StreamingStress PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.streamingstress")

set_target_properties(StreamingStress PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "StreamingStress"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(StreamingStress)
set_default_target_setting(StreamingStress)
