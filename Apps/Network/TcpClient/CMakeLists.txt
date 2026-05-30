add_executable(TcpClient Main.cpp)
target_link_libraries(TcpClient PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.tcpclient")

set_target_properties(TcpClient PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "TCP Client App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
