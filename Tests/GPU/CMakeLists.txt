# The device-free half: the shader EDSL walked into MSL and HLSL text, checked
# as strings. It links eacp-gpu-codegen and nothing else, so it builds and runs
# on every platform — including one with no GPU, no display and no graphics
# library, which is the whole point of the target existing.
#
# nano_add_executable's default main() is deliberate: these need no event loop,
# and NanoTest runs the binary with --list-tests at build time, so it has to
# start on a bare CI container.
nano_add_executable(GPUCodegenTests
        SOURCES
            AtomicCodegenTests.cpp
            ShaderCodegenTests.cpp
        TARGETS eacp-gpu-codegen)

# With the SPIR-V compiler around, every emitted GLSL source is also compiled by
# glslang, so a dialect regression fails on macOS and Windows CI rather than
# waiting for a Vulkan device. Without it the checks compile to nothing and the
# string assertions stand alone.
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

set(gpu_test_sources
        AtomicTests.cpp
        BufferRangeTests.cpp
        GPUSmokeTests.cpp
        IndirectDispatchTests.cpp
        MultiDeviceTests.cpp
        PackedHalfTests.cpp
        ShaderCompileTests.cpp
        SharedMemoryTests.cpp
        StorePlacementTests.cpp
        StreamingBufferTests.cpp
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
            BaseVertexTests.cpp
            VertexFormatTests.cpp
            BackingScaleTests.cpp
            TextureRegionTests.cpp
            TextureSamplingTests.cpp
            ShaderBlendTests.cpp
            BlendStateTests.cpp
            CullModeTests.cpp
            CoordinateSpaceTests.cpp
            SpriteBatchTests.cpp
            RenderTargetTests.cpp
            RenderTargetDepthTests.cpp
            MultisampledTargetTests.cpp
            DepthActionTests.cpp
            DepthTextureTests.cpp
            FrameComputeTests.cpp
            FrameTimingTests.cpp
            PipelineStateTests.cpp
            StencilTests.cpp
            ViewportTests.cpp
            MipmapTests.cpp
            SuppliedMipChainTests.cpp
            CompressedTextureTests.cpp
            TextureReadTests.cpp
            CubeTextureTests.cpp
            ExternalGeometryTests.cpp
            StreamedRangeDrawTests.cpp)
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
