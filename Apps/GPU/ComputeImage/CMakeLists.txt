eacp_add_app(ComputeImage Main.cpp
        DESCRIPTION "A compute kernel paints a texture that the next render pass samples.")
target_link_libraries(ComputeImage PRIVATE eacp-gpu)
set_target_properties(ComputeImage PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(ComputeImage)
