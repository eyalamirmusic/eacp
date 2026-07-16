set(CMAKE_FOLDER Examples/Graphics)

add_subdirectory(GUI)
add_subdirectory(ComplexGUI)
add_subdirectory(Todo)

if (NOT IOS)
    add_subdirectory(EmbeddedViewDemo)
    add_subdirectory(IpcDemo)
    add_subdirectory(TrayApp)

    # Snapshot capture is portable (macOS + Windows); the Screen mode falls back
    # gracefully where it is not yet available.
    add_subdirectory(VideoRecorderDemo)
endif ()
