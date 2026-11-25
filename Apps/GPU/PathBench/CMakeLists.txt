eacp_add_app(PathBench Main.cpp NESTED_PUMP CONSOLE
        DESCRIPTION "What path rasterization costs on the CPU and on the GPU.")
target_link_libraries(PathBench PRIVATE eacp-gpuwidgets)

set_default_target_setting(PathBench)
