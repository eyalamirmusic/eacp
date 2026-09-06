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
# glslang, so a dialect regression fails on every Linux lane - the two with no
# Vulkan device included - and on any other platform that opts into
# EACP_BUILD_SPIRV. Without it the checks compile to nothing and the string
# assertions stand alone, which is the default on macOS and Windows.
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

# Everything here runs on every backend. That now includes the drawing half:
# the snapshot path each of these rides - a GPUView rendered through an
# off-screen Frame and read back with View::renderToImage - is Metal textures on
# Apple, D3D12 off-screen render targets on Windows and a Vulkan render target
# on Linux, and every case self-skips without a device.
#
# The drawing files are here rather than behind a platform test because a
# scissor, a viewport, a blend mode or a cull mode has no CPU-side observable:
# the only way to check one is to draw through it and compare the pixels that
# come back, which is why so much of this directory needs a real frame.
#
# Only TextureInteropTests.mm is platform-gated, below, and it is gated on the
# API it wraps rather than on a backend's maturity.
set(gpu_test_sources
        AtomicTests.cpp
        BackingScaleTests.cpp
        BaseVertexTests.cpp
        BlendStateTests.cpp
        BufferAccessTests.cpp
        BufferRangeTests.cpp
        CompressedTextureTests.cpp
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

# The one file here that needs a display: a GPUView presenting through a Vulkan
# swapchain onto a Wayland surface. Every case self-skips without a compositor
# (and fails instead under EACP_REQUIRE_DISPLAY=1), so it is safe on the
# headless lane - it is Linux-only because a swapchain over a wl_surface is, not
# because the other backends lack the feature.
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
