jadd_executable(TcpServer Main.cpp)
target_link_libraries(TcpServer PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.tcpserver")

set_target_properties(TcpServer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "TCP Server App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
