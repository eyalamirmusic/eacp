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
