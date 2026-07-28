add_executable(PathBench Main.cpp)
target_link_libraries(PathBench PRIVATE eacp-gpuwidgets)

set_default_target_setting(PathBench)
