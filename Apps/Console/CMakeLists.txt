add_executable(Console Main.cpp)
target_link_libraries(Console PRIVATE eacp-core)

if (APPLE)
    set_target_properties(Console PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Console App"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.eacp.consoleapp")
endif ()