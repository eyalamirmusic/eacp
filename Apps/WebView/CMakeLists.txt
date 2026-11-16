set(CMAKE_FOLDER Examples/WebView)

add_subdirectory(SimpleView)
add_subdirectory(Browser)

# Options::applicationNameForUserAgent, backForwardGestures, pinchToZoom
# and elementFullscreen, all on in one window.
add_subdirectory(BrowserOptions)

add_subdirectory(FancyWindow)
add_subdirectory(WebViewEmbed)
add_subdirectory(WebViewReactAnim)
add_subdirectory(WebViewTodo)

# WebView::focused: two WebViews in one window, zoom acts on the focused one.
add_subdirectory(FocusedZoom)

add_subdirectory(WebViewSubApi)
add_subdirectory(WebViewSubApiReact)
add_subdirectory(WebViewCamera)
add_subdirectory(EmbeddedPopups)
add_subdirectory(WebViewDragOut)
add_subdirectory(WebViewToggle)

# Options::userAgent and Options::loadDeclinedPopupsInline, tested by one page.
add_subdirectory(UserAgent)
