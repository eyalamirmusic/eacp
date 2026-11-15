add_executable(ZipDemo Main.cpp)
target_link_libraries(ZipDemo PRIVATE eacp-core)

if (EMSCRIPTEN)
    eacp_web_executable(ZipDemo)
endif ()
