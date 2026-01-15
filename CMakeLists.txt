cmake_minimum_required(VERSION 3.31)
project(eacp LANGUAGES CXX OBJCXX)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=full")
endif()

function(set_default_warnings_level target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

function(set_default_target_setting target)
    set_default_warnings_level(${target})
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endfunction()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(Lib)
add_subdirectory(Apps)