add_library(eacp-network STATIC)

target_sources(eacp-network PRIVATE
        Http.cpp)

target_link_libraries(eacp-network PUBLIC eacp-core)

if (APPLE)
    target_sources(eacp-network PRIVATE
            Http.mm)
    target_link_libraries(eacp-network PRIVATE
            "-framework Foundation")
elseif (WIN32)
    target_sources(eacp-network PRIVATE
            Http-Windows.cpp)
    target_link_libraries(eacp-network PRIVATE windowsapp)
endif ()

set_default_target_setting(eacp-network)
eacp_enable_unity_build(eacp-network)
add_ide_sources(eacp-network)
