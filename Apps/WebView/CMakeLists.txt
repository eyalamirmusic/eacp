set(CMAKE_FOLDER Examples/WebView)

add_subdirectory(SimpleView)

# Its address bar is a TextInput, part of the platform 2D tier.
if (EACP_HAS_CONTEXT)
    add_subdirectory(Browser)
endif ()

add_subdirectory(FancyWindow)
add_subdirectory(WebViewCamera)
add_subdirectory(EmbeddedPopups)
add_subdirectory(WebViewDragOut)

# The apps whose page is a Vite project build it with node at build time, and
# their configure looks node up as REQUIRED, so they are added only where it is
# installed.
find_program(EACP_WEBVIEW_APPS_NODE node)

if (EACP_WEBVIEW_APPS_NODE)
    add_subdirectory(WebViewEmbed)
    add_subdirectory(WebViewReactAnim)
    add_subdirectory(WebViewTodo)
    add_subdirectory(WebViewSubApi)
    add_subdirectory(WebViewSubApiReact)
    add_subdirectory(WebViewToggle)
else ()
    message(STATUS
            "WebView: node not found, so the Vite example apps (WebViewEmbed, "
            "WebViewReactAnim, WebViewTodo, WebViewSubApi, WebViewSubApiReact, "
            "WebViewToggle) are skipped")
endif ()

# Options::userAgent and Options::loadDeclinedPopupsInline, tested by one page.
# Its buttons are drawn through a Context with a Font, the platform 2D tier.
if (EACP_HAS_CONTEXT)
    add_subdirectory(UserAgent)
endif ()
