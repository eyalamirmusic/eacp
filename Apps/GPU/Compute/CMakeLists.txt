eacp_add_app(Compute Main.cpp)
target_link_libraries(Compute PRIVATE eacp-gpu)
set_target_properties(Compute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(Compute)
