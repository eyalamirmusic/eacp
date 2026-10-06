add_subdirectory(CameraViewDemo)

# A console probe, which an Android app cannot be.
if (NOT ANDROID)
    add_subdirectory(CaptureProbe)
endif ()

# Spawns a second instance of itself, which neither iOS nor Android can do.
if (NOT IOS AND NOT ANDROID)
    add_subdirectory(IpcCameraDemo)
endif ()
