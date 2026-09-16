set(CMAKE_FOLDER Examples/Plugins)

# These two paint through the platform's own 2D tier.
if (EACP_HAS_CONTEXT)
    add_subdirectory(DemoPlugin)
    add_subdirectory(PluginHost)
endif ()

# The X11 embedding path a DAW uses: a host window's id handed across a C ABI
# to a plugin copy that draws into it with the GPU and is pumped by the host's
# loop. A window and a device are the whole of what it needs.
if (LINUX AND EACP_HAS_GPU)
    add_subdirectory(X11Plugin)
    add_subdirectory(X11Host)
endif ()
