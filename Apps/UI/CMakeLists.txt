set(CMAKE_FOLDER Examples/UI)

# A widget tree inside one GPUView: component-tier layout, painting, clipping
# and hit testing, with the batch-break count reported on screen.
add_subdirectory(ComponentTree)

# The same tier asking for more vector shapes than the shared coverage atlas can
# hold, which is the one limit in it that a real interface can reach.
add_subdirectory(AtlasCeiling)
