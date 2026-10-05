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

# A 16:9 canvas under a header and beside an inspector that keep their own
# size: WindowOptions::sizeConstraint with an AspectRatioLock, the one object
# the window enforces and the layout reads, so nothing answers a resize.
add_subdirectory(AspectLock)

# The stock widgets under one tab bar: every callback of every control written
# into a log beside them, so a gesture is one start and one end around the
# values it produced -- and a list of ten thousand rows that is one component,
# because a row is something a model paints rather than something the tree holds.
add_subdirectory(WidgetGallery)

# The OnlineResources registry as a list: DownloadAndPlay's clips, each with
# whether it is on disk, a progress bar while one is fetched, and one button
# that empties the folder both apps share.
add_subdirectory(ResourceMonitor)

# Every finger as a disc labelled with its id, position and radius, the count
# and the safe-area insets in a corner: View's touch API, with the mouse as one
# finger on a desktop.
add_subdirectory(TouchDemo)

# Every control of a game controller doing something visible, through
# GameInput's gamepads: a disc the sticks move and aim, triggers that fire and
# shield, buttons that each do a thing and announce it, and a panel of all
# fifteen buttons and six axes named as the controller's family prints them.
add_subdirectory(GamepadDemo)

# A gallery of SVG documents through the component tier, one at a time, with
# what each one costs the coverage atlas read out beneath it.
add_subdirectory(SVGDocument)
