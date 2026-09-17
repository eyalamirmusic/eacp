set(CMAKE_FOLDER Examples/Plugins)

# Host and plugin each draw a GPUView, so a window and a device are the whole
# of what the pair needs - the same everywhere the graphics modules build.
add_subdirectory(DemoPlugin)
add_subdirectory(PluginHost)

# The X11 embedding path a DAW uses: a host window's id handed across a C ABI
# to a plugin copy that draws into it with the GPU and is pumped by the host's
# loop.
if (LINUX)
    add_subdirectory(X11Plugin)
    add_subdirectory(X11Host)
endif ()
