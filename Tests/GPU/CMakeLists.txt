nano_add_executable(GPUTests
        NO_MAIN
        SOURCES
            GPUSmokeTests.cpp
            ShaderCodegenTests.cpp
            TestMain.cpp
        TARGETS eacp-gpu)
