add_library(eacp-sprites STATIC)

# Sprites builds on the GPU module's portable public API (shader EDSL, textures,
# render pass) only, so every source is platform-agnostic C++ - no APPLE/WIN32
# branches. eacp-gpu links eacp-graphics publicly, so the Graphics primitives
# (Rect, Color) come through transitively.
target_link_libraries(eacp-sprites PUBLIC eacp-gpu)

target_sources(eacp-sprites PRIVATE
        SpriteRenderer.cpp)

set_default_target_setting(eacp-sprites)
eacp_enable_unity_build(eacp-sprites)
add_ide_sources(eacp-sprites)
