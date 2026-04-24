add_library(eacp-server STATIC)

target_link_libraries(eacp-server PUBLIC eacp-network)

if (APPLE)
    target_sources(eacp-server PRIVATE
            HttpServer.mm)
    target_link_libraries(eacp-server PRIVATE
            "-framework CoreFoundation")
endif ()

set_default_target_setting(eacp-server)
eacp_enable_unity_build(eacp-server)
add_ide_sources(eacp-server)
