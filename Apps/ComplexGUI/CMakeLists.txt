add_executable(TaskBoard TaskBoard.cpp)
target_link_libraries(TaskBoard PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.taskboard")

set_target_properties(TaskBoard PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Task Board"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(TaskBoard)
