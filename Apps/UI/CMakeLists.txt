set(CMAKE_FOLDER Examples/UI)

# A widget tree inside one GPUView: component-tier layout, painting, clipping
# and hit testing, with the batch-break count reported on screen.
add_subdirectory(ComponentTree)

# The same tier asking for more vector shapes than the shared coverage atlas can
# hold, which is the one limit in it that a real interface can reach.
add_subdirectory(AtlasCeiling)

# An SVG document drawn twice in one window: native shape layers on the left,
# the component tier's coverage masks on the right, for comparing the two on
# screen and for reading what a real document costs the atlas.
add_subdirectory(SVGDocument)
