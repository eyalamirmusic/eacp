eacp_add_app(PathQuality Main.cpp)
target_link_libraries(PathQuality PRIVATE eacp-gpuwidgets)
set_target_properties(PathQuality PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(PathQuality)
