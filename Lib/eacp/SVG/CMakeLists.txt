add_library(eacp-svg STATIC)

set_default_target_setting(eacp-svg)
add_ide_sources(eacp-svg)

target_sources(eacp-svg PRIVATE SVG.cpp)

target_link_libraries(eacp-svg PUBLIC eacp-graphics)
