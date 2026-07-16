add_subdirectory(CaptureProbe)
add_subdirectory(CameraViewDemo)

# Spawns a second instance of itself, which iOS cannot do.
if (NOT IOS)
    add_subdirectory(IpcCameraDemo)
endif ()
