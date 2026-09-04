set(CMAKE_FOLDER Examples/UI)

# A widget tree inside one GPUView: component-tier layout, painting, clipping
# and hit testing, with the batch-break count reported on screen.
add_subdirectory(ComponentTree)

# The same tier asking for more vector shapes than the shared coverage atlas can
# hold, which is the one limit in it that a real interface can reach.
add_subdirectory(AtlasCeiling)

# Pictures in the tier: one image drawn at every size down from its own, a row
# of icons out of one texture, a photo under a rounded clip, and the draw count
# each of them cost.
add_subdirectory(Images)

# The blurred rounded box every card, dialog and dropdown sits on, at rising
# blurs, spreads and radii -- and the batch break it does not cost, a shadow
# being the same distance field a fill is read through a wider ramp.
add_subdirectory(Shadows)

# The one thing in the tier that is not upright: a layer composited through a
# matrix -- turned, scaled, skewed, and one inside another -- with a card
# spinning on a count of no layers rendered, a transform being read where the
# quad is drawn rather than where the content was.
add_subdirectory(Transforms)

# The stock widgets under one tab bar: every callback of every control written
# into a log beside them, so a gesture is one start and one end around the
# values it produced -- and a list of ten thousand rows that is one component,
# because a row is something a model paints rather than something the tree holds.
add_subdirectory(WidgetGallery)

# An SVG document drawn twice in one window: native shape layers on the left,
# the component tier's coverage masks on the right, for comparing the two on
# screen and for reading what a real document costs the atlas.
add_subdirectory(SVGDocument)
