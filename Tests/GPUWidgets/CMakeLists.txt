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

# The prefix sum under D3D12 GPU-based validation, which must be switched on
# before the device is created - so a binary of its own, with its own main,
# rather than a mode every other suite here would pay for.
if (WIN32)
    nano_add_executable(GPUWidgetsValidationTests
            NO_MAIN
            SOURCES
                PrefixSumValidationTests-Windows.cpp
            TARGETS eacp-gpuwidgets)

    target_link_libraries(GPUWidgetsValidationTests PRIVATE d3d12 dxgi)
endif ()
