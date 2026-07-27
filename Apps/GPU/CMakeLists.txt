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
add_subdirectory(Texture)
add_subdirectory(Maze)
add_subdirectory(Sprites)
add_subdirectory(Blending)
add_subdirectory(Instancing)

# Scissor-clipped scrolling panes: setScissorRect + wheel events + backingScale.
add_subdirectory(Clipping)

# Incremental sub-region texture uploads, the glyph-atlas pattern.
add_subdirectory(TextureAtlas)

# Text drawn from eacp-text's glyph atlas, placed by per-glyph bearings.
add_subdirectory(GlyphAtlas)

# Links eacp-video (macOS-only: AVFoundation + ScreenCaptureKit).
if (APPLE AND NOT IOS)
    add_subdirectory(VideoRecorderGpu)
endif ()
