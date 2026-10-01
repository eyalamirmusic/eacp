set(gpu_widgets_test_sources
        CoverageBatchTests.cpp
        PathKernelCpuTests.cpp
        PathTests.cpp
        PathRasterizerTests.cpp
        PathStrokerTests.cpp
        PathTessellatorTests.cpp
        PrefixSumTests.cpp
        ShaderPipelineTests.cpp)

# eacp-cpu-compute runs the path kernels on the CPU, so PathKernelCpuTests.cpp
# and PrefixSumTests.cpp can hold each one against the GPU's own buffers.
nano_add_executable(GPUWidgetsTests
        SOURCES
            ${gpu_widgets_test_sources}
        TARGETS eacp-gpuwidgets eacp-cpu-compute)
