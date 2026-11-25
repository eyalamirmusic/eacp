# main() makes blocking requests, each waiting in a nested pump.
eacp_add_app(Http Main.cpp NESTED_PUMP
        BUNDLE_ID com.eacp.httprequest
        DISPLAY_NAME "Http Request App"
        DESCRIPTION "One blocking HTTP call of each kind against httpbingo.org.")
target_link_libraries(Http PRIVATE eacp-network)
set_target_properties(Http PROPERTIES MACOSX_BUNDLE TRUE)
