set(CMAKE_FOLDER Examples/WebView)

add_subdirectory(SimpleView)
add_subdirectory(Browser)
add_subdirectory(FancyWindow)
add_subdirectory(WebViewEmbed)
add_subdirectory(WebViewReactAnim)
add_subdirectory(WebViewTodo)
add_subdirectory(WebViewSubApi)
add_subdirectory(WebViewSubApiReact)
add_subdirectory(WebViewCamera)
add_subdirectory(EmbeddedPopups)
add_subdirectory(WebViewDragOut)
add_subdirectory(WebViewToggle)

# Options::userAgent and Options::loadDeclinedPopupsInline, tested by one page.
add_subdirectory(UserAgent)

# Options::forwardUnhandledKeys by hand: every key's page verdict and the
# KeyCode that reached the native side, beside a long page for Page Up/Down.
add_subdirectory(WebViewKeys)
