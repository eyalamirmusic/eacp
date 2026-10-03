eacp_add_app(Triangle Main.cpp)
target_link_libraries(Triangle PRIVATE eacp-gpu)

find_package(ResEmbed REQUIRED)
res_embed_add(Triangle
        FILES Triangle.metal Triangle.hlsl Triangle.glsl
        NAMESPACE TriangleShaders
        CATEGORY TriangleShaders)

set_target_properties(Triangle PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Triangle)
set_default_target_setting(Triangle)
