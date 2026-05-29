add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

find_package(EADataStructures REQUIRED)
target_link_libraries(eacp-core PUBLIC ea_data_structures)

target_sources(eacp-core PRIVATE
        App/App.cpp
        Threads/EventLoop.cpp
        Threads/TaskSemaphore.cpp
        Utils/File.cpp
        Utils/Files.cpp
        Utils/Logging.cpp
        Utils/Random.cpp
        Utils/Strings.cpp)

if (APPLE)
    target_sources(eacp-core PRIVATE
            ObjC/AutoReleasePool.mm
            ObjC/Strings.mm
            Threads/EventLoop.mm
            Threads/Timer.mm
            Threads/ThreadUtils.mm
            Utils/Files.mm)
    target_link_libraries(eacp-core PRIVATE
            "-framework Foundation")
    if (IOS)
        target_sources(eacp-core PRIVATE
                App/App-iOS.mm
                Threads/EventLoop-iOS.mm)
        target_link_libraries(eacp-core PRIVATE "-framework UIKit")
    else ()
        target_sources(eacp-core PRIVATE
                App/App-macOS.mm
                Threads/EventLoop-macOS.mm)
        target_link_libraries(eacp-core PRIVATE
                "-framework Cocoa"
                "-framework UniformTypeIdentifiers")
    endif ()
elseif (WIN32)
    target_sources(eacp-core PRIVATE
            App/App-Windows.cpp
            Threads/EventLoop-Windows.cpp
            Threads/ThreadUtils-Windows.cpp
            Threads/Timer-Windows.cpp
            Utils/Files-Windows.cpp)
    target_link_libraries(eacp-core PRIVATE windowsapp)
elseif (UNIX AND NOT APPLE)
    target_sources(eacp-core PRIVATE
            App/App-Linux.cpp
            Threads/EventLoop-Linux.cpp
            Threads/ThreadUtils-Linux.cpp
            Threads/Timer-Linux.cpp
            Utils/Files-Linux.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(eacp-core PRIVATE Threads::Threads)
endif ()

set_default_target_setting(eacp-core)
eacp_enable_unity_build(eacp-core)
add_ide_sources(eacp-core)

add_subdirectory(Testing)
