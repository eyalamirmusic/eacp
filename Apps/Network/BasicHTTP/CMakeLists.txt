add_executable(BasicHttp Main.cpp)
target_link_libraries(BasicHttp PRIVATE eacp-network Miro)

set(BUNDLE_ID "com.eacp.basichttprequest")

set_target_properties(BasicHttp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Basic Http Request App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

# main() makes blocking requests, each waiting in a nested pump.
if (EMSCRIPTEN)
    eacp_web_executable(BasicHttp NESTED_PUMP)
endif ()
