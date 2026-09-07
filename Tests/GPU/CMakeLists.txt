# The device-free half: the shader EDSL walked into MSL and HLSL text and
# checked as strings, so it runs with no GPU, display or graphics library.
nano_add_executable(GPUCodegenTests
        SOURCES
            AtomicCodegenTests.cpp
            ShaderCodegenTests.cpp
        TARGETS eacp-gpu-codegen)

# With eacp-spirv around, every emitted GLSL source is compiled by glslang too;
# without it the checks compile to nothing and the string assertions stand alone.
function(eacp_enable_glsl_compile_checks target)
    if (TARGET eacp-spirv)
        target_link_libraries(${target} PRIVATE eacp-spirv)
        target_compile_definitions(${target} PRIVATE EACP_HAS_SPIRV=1)
    endif ()
endfunction()

eacp_enable_glsl_compile_checks(GPUCodegenTests)

if (NOT EACP_HAS_GPU)
    return ()
endif ()

# Everything here runs on every backend, drawing included: a scissor or a blend
# mode has no CPU-side observable. Every case self-skips without a device.
set(gpu_test_sources
        AtomicTests.cpp
        BackingScaleTests.cpp
        BaseVertexTests.cpp
        BlendStateTests.cpp
        BufferAccessTests.cpp
        BufferRangeTests.cpp
        CompressedTextureTests.cpp
        ComputeBufferRangeTests.cpp
        CoordinateSpaceTests.cpp
        CubeTextureTests.cpp
        CullModeTests.cpp
        DepthActionTests.cpp
        DepthTextureTests.cpp
        DevicePresenceTests.cpp
        ExternalGeometryTests.cpp
        FrameComputeTests.cpp
        FrameTimingTests.cpp
        GPUSmokeTests.cpp
        GPUSnapshotTests.cpp
        IndirectDispatchTests.cpp
        IntrinsicTests.cpp
        MipmapTests.cpp
        MultiDeviceTests.cpp
        MultisampledTargetTests.cpp
        PackedHalfTests.cpp
        PipelineStateTests.cpp
        RenderTargetDepthTests.cpp
        RenderTargetTests.cpp
        ScissorTests.cpp
        ShaderBlendTests.cpp
        ShaderCompileTests.cpp
        SharedMemoryTests.cpp
        SpriteBatchTests.cpp
        StencilTests.cpp
        StorePlacementTests.cpp
        StreamedRangeDrawTests.cpp
        StreamingBufferTests.cpp
        SuppliedMipChainTests.cpp
        TextureCreationTests.cpp
        TextureReadTests.cpp
        TextureRegionTests.cpp
        TextureSamplingTests.cpp
        TextureUpdateTests.cpp
        VertexFormatTests.cpp
        ViewportTests.cpp
        TestMain.cpp)

# The zero-copy CVPixelBuffer wrap test is Metal-only Objective-C++.
if (APPLE)
    list(APPEND gpu_test_sources TextureInteropTests.mm)
endif ()

# The one file that needs a display: a GPUView presenting through a Vulkan
# swapchain onto a Wayland surface. Self-skips without a compositor.
if (LINUX)
    list(APPEND gpu_test_sources PresentTests-Linux.cpp)
endif ()

# eacp-sprites is linked because CoordinateSpaceTests draws real rects through
# SpriteRenderer: the y-axis contract it pins is between Rect and the things
# that draw it, so it cannot be checked without one of them. SpriteBatchTests
# rides the same path to pin what the renderer's batching may not reorder.
nano_add_executable(GPUTests
        NO_MAIN
        SOURCES
            ${gpu_test_sources}
        TARGETS eacp-sprites eacp-gpu)

eacp_enable_glsl_compile_checks(GPUTests)

if (APPLE)
    target_link_libraries(GPUTests PRIVATE "-framework CoreVideo")
endif ()
