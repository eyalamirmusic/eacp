cmake_minimum_required(VERSION 3.31)
project(eacp LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if (APPLE)
    enable_language(OBJCXX)
endif ()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "LK9GL8NWU4" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer" CACHE STRING "" FORCE)
    set(EACP_IOS TRUE)
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
    set(EACP_IOS FALSE)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=full")
endif()

function(set_default_warnings_level target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(set_default_target_setting target)
    set_default_warnings_level(${target})
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endfunction()

function(add_ide_sources target)
    file(GLOB_RECURSE ALL_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    )

    list(FILTER ALL_SOURCES EXCLUDE REGEX "^${CMAKE_CURRENT_SOURCE_DIR}/[^/]+\\.(cpp|mm)$")

    if(ALL_SOURCES)
        target_sources(${target} PRIVATE ${ALL_SOURCES})
        set_source_files_properties(${ALL_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(Lib)
add_subdirectory(Apps)