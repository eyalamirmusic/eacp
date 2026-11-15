add_subdirectory(CameraViewDemo)

# A console probe, which an Android app cannot be.
if (NOT ANDROID)
    add_subdirectory(CaptureProbe)
endif ()

# Spawns a second instance of itself, which neither iOS nor Android can do, and
# ships the frames to it over IPC.
if (EACP_HAS_SOCKETS AND NOT IOS AND NOT ANDROID)
    add_subdirectory(IpcCameraDemo)
endif ()
