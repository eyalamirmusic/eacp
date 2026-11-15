add_executable(OnlineResourceDemo Main.cpp)
target_link_libraries(OnlineResourceDemo PRIVATE eacp-network)

# main() blocks in OnlineResource::fetch, which waits in a nested pump.
if (EMSCRIPTEN)
    eacp_web_executable(OnlineResourceDemo NESTED_PUMP)
endif ()
