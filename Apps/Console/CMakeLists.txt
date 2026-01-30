add_executable(Console Main.cpp)
target_link_libraries(Console PRIVATE eacp-core)

set(BUNDLE_ID "com.eacp.consoleapp")

if (APPLE)
    set_target_properties(Console PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Console App"
            MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
endif ()