set(CMAKE_FOLDER Examples/Mixed)

add_subdirectory(LayeredViews)
add_subdirectory(MixedViews)

# The same lines of text in the system web view and in the component tier's
# TextRenderer, side by side at the same faces, sizes, weights and positions:
# the comparison the tier's text is judged against, and the place to look when
# it reads thinner or softer than the rest of the platform.
add_subdirectory(TextBesideWebView)
