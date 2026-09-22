add_library(eacp-sa3-dit STATIC)

target_link_libraries(eacp-sa3-dit PUBLIC eacp-ml)

target_sources(eacp-sa3-dit PRIVATE
        Ops.cpp
        SA3DiT.cpp
        Weights.cpp)

set_default_target_setting(eacp-sa3-dit)
eacp_enable_unity_build(eacp-sa3-dit)
add_ide_sources(eacp-sa3-dit)
