cmake_minimum_required(VERSION 3.31)
project(eacp LANGUAGES CXX OBJCXX)

function(set_default_warnings_level target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(Lib)
add_subdirectory(Apps)