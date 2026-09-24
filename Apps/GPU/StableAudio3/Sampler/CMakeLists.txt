add_library(eacp-sa3-sampler STATIC)

target_include_directories(eacp-sa3-sampler PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)

target_link_libraries(eacp-sa3-sampler PUBLIC eacp-sa3-dit)

target_sources(eacp-sa3-sampler PRIVATE
        Sampler.cpp)

set_default_target_setting(eacp-sa3-sampler)
eacp_enable_unity_build(eacp-sa3-sampler)
add_ide_sources(eacp-sa3-sampler)
