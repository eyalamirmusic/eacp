set(gpu_test_sources
        GPUSmokeTests.cpp
        ShaderCodegenTests.cpp
        TestMain.cpp
        TextureUpdateTests.cpp)

# The zero-copy CVPixelBuffer wrap test is Metal-only Objective-C++.
if (APPLE)
    list(APPEND gpu_test_sources TextureInteropTests.mm)
endif ()

# GPU snapshot read-back (View::renderToImage over a GPUView): Metal off-screen
# textures on Apple, D3D12 off-screen render targets on Windows. The scissor
# tests ride the same off-screen path — a scissor rect has no CPU-side
# observable, so the only way to check one is to draw through it and read the
# pixels back.
if (APPLE OR WIN32)
    list(APPEND gpu_test_sources
            GPUSnapshotTests.cpp
            ScissorTests.cpp
            BackingScaleTests.cpp
            TextureRegionTests.cpp
            ShaderBlendTests.cpp)
endif ()

nano_add_executable(GPUTests
        NO_MAIN
        SOURCES
            ${gpu_test_sources}
        TARGETS eacp-gpu)

if (APPLE)
    target_link_libraries(GPUTests PRIVATE "-framework CoreVideo")
endif ()
