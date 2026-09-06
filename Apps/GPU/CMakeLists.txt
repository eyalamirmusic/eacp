set(CMAKE_FOLDER Examples/GPU)

add_subdirectory(Triangle)
add_subdirectory(TriangleGen)
add_subdirectory(RotatingTriangle)
add_subdirectory(Teapot)
add_subdirectory(Compute)

# Compute feeding a draw on one frame: Frame::beginCompute, and a state buffer
# the CPU never reads back.
add_subdirectory(ComputeParticles)

# A kernel painting a texture the next pass samples full-screen: 2D dispatch
# via threadPosition() and TextureDescriptor::computeWrite.
add_subdirectory(ComputeImage)

# Non-blocking submission: CommandBuffer::commitAsync overlapping GPU work with
# the CPU, measured against the blocking commit.
add_subdirectory(AsyncCompute)
add_subdirectory(Paths)

# Analytic antialiasing in a kernel: coverage computed per pixel from a path's
# segments, with no multisampling anywhere.
add_subdirectory(PathCoverage)

# Headless: what a rasterization costs on the CPU against what it costs on the
# GPU, which is the split that decides what is worth moving between them.
add_subdirectory(PathBench)
add_subdirectory(Texture)
add_subdirectory(Maze)
add_subdirectory(Sprites)

# Scissor-clipped scrolling panes: setScissorRect + wheel events + backingScale.
add_subdirectory(Clipping)

# Two thousand meshes a frame, each written as its own slice: StreamingBuffers
# under a game renderer's load - the draw order shuffled every frame and the
# byte total doubled every three seconds, with the allocation count that says
# the arenas are recycling rather than churning.
add_subdirectory(StreamingStress)

# Incremental sub-region texture uploads, the glyph-atlas pattern.
add_subdirectory(TextureAtlas)

# Everything from here to the end paints an overlay with the 2D
# Graphics::Context - a caption, a legend, a Path drawn by the platform's own
# rasterizer beside the GPU's - so each needs the whole 2D and glyph stack
# rather than only a window, which is why none of them is in the Linux build
# while EACP_HAS_TEXT is off there. The GPU half of each would build; the
# labels are what would not.
if (EACP_HAS_TEXT)
    # Blend modes side by side, and instanced quads beside them - each panel
    # captioned with what it should look like.
    add_subdirectory(Blending)
    add_subdirectory(Instancing)

    # Stencil shadow volumes: per-face stencil ops counting depth failures, and
    # the lighting pass masked on the count. The algorithm the stencil buffer is
    # for.
    add_subdirectory(StencilShadows)

    # Cube textures: a sky sampled by view direction and a sphere reflecting it.
    # Where the six faces go, and what a direction lookup is.
    add_subdirectory(CubeMap)

    # One pass reading the depth another wrote: sampleableDepth +
    # DepthAction::Keep + depthTexture(), and the soft particles that is all
    # for. R32Float beside it.
    add_subdirectory(DepthSampling)

    # The same shapes filled three ways side by side (that kernel, ear clipping
    # under 4x MSAA, and the platform's own rasterizer), and strokes as fills
    # beside the platform's own stroker. Both compare the GPU rasterizer against
    # Graphics::Context::fillPath and strokePath, so the comparison is exactly
    # the half that needs the 2D stack.
    add_subdirectory(PathQuality)
    add_subdirectory(PathStroke)

    # The two that link eacp-text rather than only the GPU module: text drawn
    # from the glyph atlas with per-glyph bearings, and one embedded variable
    # face at nine weights beside a family cut into faces - registerMemoryFont,
    # and a weight asked of an axis rather than matched.
    add_subdirectory(GlyphAtlas)
    add_subdirectory(VariableFont)
endif ()

# Links eacp-video (macOS-only: AVFoundation + ScreenCaptureKit).
if (APPLE AND NOT IOS)
    add_subdirectory(VideoRecorderGpu)
endif ()
