project(eacp_lib LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD 20)

add_library(eacp STATIC)
target_include_directories(eacp PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(eacp PRIVATE
        "-framework foundation")

add_subdirectory(eacp)

