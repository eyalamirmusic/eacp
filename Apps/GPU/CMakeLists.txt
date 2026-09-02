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

# The same shapes filled three ways side by side - that kernel, ear clipping
# under 4x MSAA, and the platform's own rasterizer.
add_subdirectory(PathQuality)

# Strokes as fills: overlapping quads, joins and caps unioned by the non-zero
# rule, next to the platform's own stroker.
add_subdirectory(PathStroke)

# Headless: what a rasterization costs on the CPU against what it costs on the
# GPU, which is the split that decides what is worth moving between them.
add_subdirectory(PathBench)
add_subdirectory(Texture)
add_subdirectory(Maze)
add_subdirectory(Sprites)
add_subdirectory(Blending)
add_subdirectory(Instancing)

# Scissor-clipped scrolling panes: setScissorRect + wheel events + backingScale.
add_subdirectory(Clipping)

# Stencil shadow volumes: per-face stencil ops counting depth failures, and the
# lighting pass masked on the count. The algorithm the stencil buffer is for.
add_subdirectory(StencilShadows)

# Incremental sub-region texture uploads, the glyph-atlas pattern.
add_subdirectory(TextureAtlas)

# Text drawn from eacp-text's glyph atlas, placed by per-glyph bearings.
add_subdirectory(GlyphAtlas)

# One embedded variable face drawn at nine weights beside a family cut into
# faces: registerMemoryFont, and a weight asked of an axis rather than matched.
add_subdirectory(VariableFont)

# Links eacp-video (macOS-only: AVFoundation + ScreenCaptureKit).
if (APPLE AND NOT IOS)
    add_subdirectory(VideoRecorderGpu)
endif ()
