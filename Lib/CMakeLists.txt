project(eacp_lib LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD 20)

add_subdirectory(eacp)