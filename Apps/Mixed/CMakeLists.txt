set(CMAKE_FOLDER Examples/Mixed)

# Its frame and labels are the retained shape and text layers, part of the
# platform 2D tier.
if (EACP_HAS_CONTEXT)
    add_subdirectory(LayeredViews)
endif ()

# The live camera under the web view, so only where Camera builds.
if (EACP_HAS_CAPTURE)
    add_subdirectory(MixedViews)
endif ()

# The same lines of text in the system web view and in the component tier's
# TextRenderer, side by side at the same faces, sizes, weights and positions:
# the comparison the tier's text is judged against, and the place to look when
# it reads thinner or softer than the rest of the platform.
add_subdirectory(TextBesideWebView)
