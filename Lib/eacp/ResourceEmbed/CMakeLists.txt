add_library(eacp-resource-embed STATIC)
target_include_directories(eacp-resource-embed PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)
target_link_libraries(eacp-resource-embed PRIVATE eacp-core)

set_default_target_setting(eacp-resource-embed)
add_ide_sources(eacp-resource-embed)

target_sources(eacp-resource-embed PRIVATE ResourceEmbed.cpp)
