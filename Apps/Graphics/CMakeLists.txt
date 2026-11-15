set(CMAKE_FOLDER Examples/Graphics)

# The component tier: a window and a glyph rasterizer are all these need.
if (EACP_HAS_TEXT)
    add_subdirectory(GUI)
    add_subdirectory(ComplexGUI)
    add_subdirectory(Todo)

    # Key events and the clipboard, shown live.
    add_subdirectory(KeyInspector)
endif ()

if (EACP_HAS_TEXT AND NOT IOS)
    add_subdirectory(EmbeddedViewDemo)
    add_subdirectory(TrayApp)

    # Menu items greying themselves out from live state, with the bar installed
    # once. iOS has no menu bar, so this is desktop-only.
    add_subdirectory(MenuBarApp)

    # The pointer changing shape per region of one view, which is what a
    # GPU-drawn UI needs and a per-view cursor cannot do. No pointer on iOS.
    add_subdirectory(CursorShapes)
endif ()

# Two processes, one typed RPC bridge over IPC, the second launched by the first.
if (EACP_HAS_TEXT AND EACP_HAS_SOCKETS AND NOT IOS)
    add_subdirectory(IpcDemo)
endif ()

# Snapshot capture is portable (macOS + Windows); the Screen mode falls back
# gracefully where it is not yet available.
if (EACP_HAS_TEXT AND EACP_HAS_CAPTURE AND NOT IOS)
    add_subdirectory(VideoRecorderDemo)
endif ()

# These paint through the platform's own 2D Context and its fonts.
if (EACP_HAS_CONTEXT)
    # Apps::showAlert: each icon, a cancel button, five buttons and one raised
    # from a worker thread, with the answer written beside them.
    add_subdirectory(AlertDemo)
endif ()

if (EACP_HAS_CONTEXT AND NOT IOS)
    # Apps::setAppBadge, which needs a Dock tile or a taskbar button to draw on
    # and has neither on iOS.
    add_subdirectory(AppBadge)

    # Window::getPosition / setPosition / WindowEvents::onMoved. iOS has one
    # window and it fills the screen, so there is nothing to place.
    add_subdirectory(WindowPosition)
endif ()
