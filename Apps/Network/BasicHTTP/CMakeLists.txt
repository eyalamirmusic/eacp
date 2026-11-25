# main() makes blocking requests, each waiting in a nested pump.
eacp_add_app(BasicHttp Main.cpp NESTED_PUMP
        BUNDLE_ID com.eacp.basichttprequest
        DISPLAY_NAME "Basic Http Request App"
        DESCRIPTION "A reflected struct POSTed as JSON ten times to httpbingo.org, echoed back.")
target_link_libraries(BasicHttp PRIVATE eacp-network Miro)
set_target_properties(BasicHttp PROPERTIES MACOSX_BUNDLE TRUE)
