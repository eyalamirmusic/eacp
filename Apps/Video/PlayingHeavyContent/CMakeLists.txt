add_executable(PlayingHeavyContent Main.cpp)
target_link_libraries(PlayingHeavyContent PRIVATE eacp-video-view eacp-text)

set(BUNDLE_ID "com.eacp.playingheavycontent")

set_target_properties(PlayingHeavyContent PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Playing Heavy Content"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(PlayingHeavyContent)
set_default_target_setting(PlayingHeavyContent)
