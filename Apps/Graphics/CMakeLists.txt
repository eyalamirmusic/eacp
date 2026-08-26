set(CMAKE_FOLDER Examples/Graphics)

add_subdirectory(GUI)
add_subdirectory(ComplexGUI)
add_subdirectory(Todo)

if (NOT IOS)
    add_subdirectory(EmbeddedViewDemo)
    add_subdirectory(IpcDemo)
    add_subdirectory(TrayApp)

    # Apps::setAppBadge, which needs a Dock tile or a taskbar button to draw on
    # and has neither on iOS.
    add_subdirectory(AppBadge)

    # Window::getPosition / setPosition / WindowEvents::onMoved. iOS has one
    # window and it fills the screen, so there is nothing to place.
    add_subdirectory(WindowPosition)

    # Menu items greying themselves out from live state, with the bar installed
    # once. iOS has no menu bar, so this is desktop-only.
    add_subdirectory(MenuBarApp)

    # The pointer changing shape per region of one view, which is what a
    # GPU-drawn UI needs and a per-view cursor cannot do. No pointer on iOS.
    add_subdirectory(CursorShapes)

    # Snapshot capture is portable (macOS + Windows); the Screen mode falls back
    # gracefully where it is not yet available.
    add_subdirectory(VideoRecorderDemo)
endif ()

# Key events and the clipboard, shown live.
add_subdirectory(KeyInspector)
