add_executable(Compute Main.cpp)
target_link_libraries(Compute PRIVATE eacp-gpu)

set_default_target_setting(Compute)
