eacp_add_app(ZipDemo Main.cpp
        DESCRIPTION "A zip archive built in memory, written, read back and unpacked.")
target_link_libraries(ZipDemo PRIVATE eacp-core)
