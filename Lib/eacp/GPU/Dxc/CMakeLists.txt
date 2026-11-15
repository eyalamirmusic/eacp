add_library(eacp-gpu-dxc STATIC)

target_sources(eacp-gpu-dxc PRIVATE DxcResource.cpp)
target_link_libraries(eacp-gpu-dxc PUBLIC eacp-gpu eacp-network)

set_default_target_setting(eacp-gpu-dxc)
eacp_enable_unity_build(eacp-gpu-dxc)
add_ide_sources(eacp-gpu-dxc)
