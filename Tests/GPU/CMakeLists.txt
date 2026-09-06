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

# Everything here runs on every backend, which on Linux means the compute and
# buffer half of the Vulkan backend and nothing else - no Texture, no
# RenderPipeline, no Frame. A file joins this list when every case in it either
# needs none of those or self-skips on the object it needs; the rest are in the
# APPLE OR WIN32 list below, each with the reason it is there.
set(gpu_test_sources
        AtomicTests.cpp
        BufferRangeTests.cpp
        DevicePresenceTests.cpp
        IndirectDispatchTests.cpp
        PackedHalfTests.cpp
        SharedMemoryTests.cpp
        StorePlacementTests.cpp
        StreamingBufferTests.cpp
        TestMain.cpp)

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
    # Four of these are here because of the Linux render half rather than
    # because of a snapshot, and each names what has to arrive before it moves
    # back to the list above:
    #
    #   GPUSmokeTests      builds a RenderPipeline and a Texture and asserts
    #                      both are valid - needs Texture and RenderPipeline.
    #   ShaderCompileTests every case compiles a generated source into a
    #                      RenderPipeline - needs RenderPipeline.
    #   TextureUpdateTests re-uploads into a Texture and asserts it stays valid
    #                      and sized - needs Texture.
    #   MultiDeviceTests   asserts two Devices have two queues. Vulkan hands
    #                      queues out of a family with a driver-decided count,
    #                      and lavapipe offers one, so two Devices there share
    #                      the VkQueue and the pointers are equal. Everything
    #                      else the file pins - independent pools, a Device on a
    #                      worker thread, the shared Device staying with the
    #                      main one - already holds on Vulkan; only that one
    #                      assertion cannot, and only on a single-queue device.
    list(APPEND gpu_test_sources
            GPUSmokeTests.cpp
            ShaderCompileTests.cpp
            TextureUpdateTests.cpp
            MultiDeviceTests.cpp
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
