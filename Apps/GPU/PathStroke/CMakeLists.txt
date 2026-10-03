eacp_add_app(PathStroke Main.cpp)
target_link_libraries(PathStroke PRIVATE eacp-gpuwidgets)
set_target_properties(PathStroke PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(PathStroke)
