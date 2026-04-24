add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

target_sources(eacp-core PRIVATE
        App/App.cpp
        Threads/EventLoop.cpp
        Threads/TaskSemaphore.cpp
        Utils/Files.cpp
        Utils/Logging.cpp
        Utils/Random.cpp)

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
        target_sources(eacp-core PRIVATE Threads/EventLoop-iOS.mm)
        target_link_libraries(eacp-core PRIVATE "-framework UIKit")
    else ()
        target_sources(eacp-core PRIVATE Threads/EventLoop-macOS.mm)
        target_link_libraries(eacp-core PRIVATE
                "-framework Cocoa")
    endif ()
elseif (WIN32)
    target_sources(eacp-core PRIVATE
            Threads/EventLoop-Windows.cpp
            Threads/ThreadUtils-Windows.cpp
            Threads/Timer-Windows.cpp
            Utils/Files-Windows.cpp)
    target_link_libraries(eacp-core PRIVATE windowsapp)
endif ()

set_default_target_setting(eacp-core)
eacp_enable_unity_build(eacp-core)
add_ide_sources(eacp-core)
