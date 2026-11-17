# main() blocks in OnlineResource::fetch, which waits in a nested pump.
eacp_add_app(OnlineResourceDemo Main.cpp NESTED_PUMP
        DESCRIPTION "Fetches one file once through OnlineResource, blocking in main().")
target_link_libraries(OnlineResourceDemo PRIVATE eacp-network)
