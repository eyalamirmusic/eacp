add_executable(Triangle Main.cpp)
target_link_libraries(Triangle PRIVATE eacp-gpu)

find_package(ResEmbed REQUIRED)
res_embed_add(Triangle
        FILES Triangle.metal Triangle.hlsl
        NAMESPACE TriangleShaders
        CATEGORY TriangleShaders)

set(BUNDLE_ID "com.eacp.triangle")

set_target_properties(Triangle PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Triangle"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Triangle)
set_default_target_setting(Triangle)
