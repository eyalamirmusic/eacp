add_subdirectory(Console)
add_subdirectory(HTTP)
add_subdirectory(GUI)
add_subdirectory(Todo)
add_subdirectory(SimpleView)

if (WIN32)
    add_subdirectory(DebugWinGraphics)
endif ()