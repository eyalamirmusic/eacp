add_executable(TcpProbe Main.cpp)
target_link_libraries(TcpProbe PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.tcpprobe")

set_target_properties(TcpProbe PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "TCP Probe App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
