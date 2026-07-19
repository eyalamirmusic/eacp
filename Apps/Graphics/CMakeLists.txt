set(CMAKE_FOLDER Examples/Graphics)

add_subdirectory(GUI)
add_subdirectory(ComplexGUI)
add_subdirectory(Todo)

if (NOT IOS)
    add_subdirectory(EmbeddedViewDemo)
    add_subdirectory(IpcDemo)
    add_subdirectory(TrayApp)

    # Menu items greying themselves out from live state, with the bar installed
    # once. iOS has no menu bar, so this is desktop-only.
    add_subdirectory(MenuBarApp)

    # Snapshot capture is portable (macOS + Windows); the Screen mode falls back
    # gracefully where it is not yet available.
    add_subdirectory(VideoRecorderDemo)
endif ()

# Key events and the clipboard, shown live.
add_subdirectory(KeyInspector)
