set(CMAKE_FOLDER Examples/GPU)

add_subdirectory(Triangle)
add_subdirectory(TriangleGen)
add_subdirectory(RotatingTriangle)
add_subdirectory(Teapot)
add_subdirectory(Compute)
add_subdirectory(Paths)
add_subdirectory(Texture)
add_subdirectory(Maze)
add_subdirectory(Sprites)
add_subdirectory(Blending)
add_subdirectory(Instancing)

# Scissor-clipped scrolling panes: setScissorRect + wheel events + backingScale.
add_subdirectory(Clipping)

# Links eacp-video (macOS-only: AVFoundation + ScreenCaptureKit).
if (APPLE AND NOT IOS)
    add_subdirectory(VideoRecorderGpu)
endif ()
