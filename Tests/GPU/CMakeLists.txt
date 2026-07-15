set(gpu_test_sources
        GPUSmokeTests.cpp
        ShaderCodegenTests.cpp
        TestMain.cpp
        TextureUpdateTests.cpp)

# The zero-copy CVPixelBuffer wrap test is Metal-only Objective-C++.
if (APPLE)
    list(APPEND gpu_test_sources TextureInteropTests.mm)

    # GPU snapshot read-back (View::renderToImage over a GPUView) is wired on the
    # Metal backend only; the Windows renderToImage is still a stub.
    list(APPEND gpu_test_sources GPUSnapshotTests.cpp)
endif ()

nano_add_executable(GPUTests
        NO_MAIN
        SOURCES
            ${gpu_test_sources}
        TARGETS eacp-gpu)

if (APPLE)
    target_link_libraries(GPUTests PRIVATE "-framework CoreVideo")
endif ()
