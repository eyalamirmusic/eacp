# Everything here runs on every backend. The two render-pipeline cases in
# ShaderPipelineTests self-skip on the device like every other GPU test, and its
# codegen case asks each dialect for its own spelling, so the file is portable
# now that the Vulkan backend builds a RenderPipeline. The rasterization checks
# in the rest of the directory go through compute and self-skip on the mask
# texture where there is none.
set(gpu_widgets_test_sources
        CoverageBatchTests.cpp
        PathTests.cpp
        PathRasterizerTests.cpp
        PathStrokerTests.cpp
        PathTessellatorTests.cpp
        PrefixSumTests.cpp
        ShaderPipelineTests.cpp)

nano_add_executable(GPUWidgetsTests
        SOURCES
            ${gpu_widgets_test_sources}
        TARGETS eacp-gpuwidgets)
