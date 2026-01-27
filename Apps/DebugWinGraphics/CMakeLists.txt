cmake_minimum_required(VERSION 3.20)
project(DirectCompositionDemo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME} WIN32
    main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dcomp
    d2d1
    ole32
    user32
    gdi32
)

# Enable warnings (compiler-specific)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()
