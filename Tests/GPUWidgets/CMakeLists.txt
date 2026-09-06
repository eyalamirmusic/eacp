set(gpu_widgets_test_sources
        CoverageBatchTests.cpp
        PathTests.cpp
        PathRasterizerTests.cpp
        PathStrokerTests.cpp
        PathTessellatorTests.cpp
        PrefixSumTests.cpp)

# The render half, which the Linux backend does not have yet (stage 3 of
# plan.md). Two of these three cases build a RenderPipeline and assert it is
# valid; the third reads the generated source and checks it against the MSL and
# HLSL spellings, which the GLSL dialect does not share. Everything else in this
# directory rasterizes through compute and runs everywhere - the coverage probe
# and the batch checks self-skip on the mask texture where there is none, the
# way every GPU test self-skips on the device.
if (APPLE OR WIN32)
    list(APPEND gpu_widgets_test_sources ShaderPipelineTests.cpp)
endif ()

nano_add_executable(GPUWidgetsTests
        SOURCES
            ${gpu_widgets_test_sources}
        TARGETS eacp-gpuwidgets)
