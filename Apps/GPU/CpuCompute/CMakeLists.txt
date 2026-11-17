eacp_add_app(CpuCompute Main.cpp NESTED_PUMP CONSOLE
        DESCRIPTION "One kernel on the GPU and on the CPU interpreter, compared and timed.")
target_link_libraries(CpuCompute PRIVATE eacp-gpu eacp-cpu-compute)
set_target_properties(CpuCompute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(CpuCompute)
