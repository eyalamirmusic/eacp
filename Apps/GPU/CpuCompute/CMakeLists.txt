eacp_add_app(CpuCompute Main.cpp)
target_link_libraries(CpuCompute PRIVATE eacp-gpu eacp-cpu-compute)
set_target_properties(CpuCompute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(CpuCompute)
