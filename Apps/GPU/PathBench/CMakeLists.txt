eacp_add_app(PathBench Main.cpp)
target_link_libraries(PathBench PRIVATE eacp-gpuwidgets-benchmark-paths)

set_default_target_setting(PathBench)
