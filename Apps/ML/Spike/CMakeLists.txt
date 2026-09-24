add_executable(MLSpike Spike.mm)
target_link_libraries(MLSpike PRIVATE eacp-core
        "-framework CoreML" "-framework CoreVideo" "-framework IOSurface"
        "-framework Accelerate")

# MLComputePlan and MLModelStructure are macOS 14.4 API.
target_compile_options(MLSpike PRIVATE -fobjc-arc -mmacosx-version-min=14.4)
target_link_options(MLSpike PRIVATE -mmacosx-version-min=14.4)
target_compile_definitions(MLSpike PRIVATE ACCELERATE_NEW_LAPACK)
eacp_skip_pch(MLSpike)
