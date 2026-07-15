set(CMAKE_FOLDER Examples/Graphics)

add_subdirectory(GUI)
add_subdirectory(ComplexGUI)
add_subdirectory(Todo)

if (NOT IOS)
    add_subdirectory(EmbeddedViewDemo)
    add_subdirectory(TrayApp)

    if (APPLE)
        add_subdirectory(VideoRecorderDemo)
    endif ()
endif ()
