# The paths Apps/GPU/PathBench and Tests/CpuCompute/CpuComputeBench both time,
# in one header-only target so neither depends on the other.
add_library(eacp-gpuwidgets-benchmark-paths INTERFACE)
target_link_libraries(eacp-gpuwidgets-benchmark-paths INTERFACE eacp-gpuwidgets)
