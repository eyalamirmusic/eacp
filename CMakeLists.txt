cmake_minimum_required(VERSION 3.31)
project(eacp LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

include(TargetSetup)

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(Lib)

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(Apps)
endif ()