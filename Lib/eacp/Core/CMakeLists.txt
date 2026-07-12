add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

find_package(EADataStructures REQUIRED)
target_link_libraries(eacp-core PUBLIC ea_data_structures)

target_sources(eacp-core PRIVATE
        App/App.cpp
        Platform/Platform.cpp
        Process/Process.cpp
        Threads/Async.cpp
        Threads/EventLoop.cpp
        Threads/TaskSemaphore.cpp
        Utils/Environment.cpp
        Utils/File.cpp
        Utils/FilePath.cpp
        Utils/Files.cpp
        Utils/Logging.cpp
        Utils/Random.cpp
        Utils/Strings.cpp)

if (APPLE)
    target_sources(eacp-core PRIVATE
            ObjC/AutoReleasePool.mm
            ObjC/Strings.mm
            Plugins/DynamicLibrary-Apple.cpp
            Plugins/DynamicLibrary-Posix.cpp
            Plugins/ModuleInfo-Apple.cpp
            Plugins/ModuleInfo-Posix.cpp
            Threads/EventLoop.mm
            Threads/EventLoop-Default.cpp
            Threads/Timer.mm
            Threads/ThreadUtils.mm
            Utils/Environment-Posix.cpp
            Utils/Files.mm
            Utils/Logging-Posix.cpp)
    target_link_libraries(eacp-core PRIVATE
            "-framework Foundation")
    if (IOS)
        target_sources(eacp-core PRIVATE
                App/App-iOS.mm
                App/Clipboard-iOS.mm
                App/LoginItem-iOS.cpp
                Process/Process-iOS.cpp
                Threads/EventLoop-iOS.mm)
        target_link_libraries(eacp-core PRIVATE "-framework UIKit")
    else ()
        target_sources(eacp-core PRIVATE
                App/App-macOS.mm
                App/Clipboard-macOS.mm
                App/LoginItem-macOS.mm
                Process/Process-Posix.cpp
                Threads/EventLoop-macOS.mm)
        target_link_libraries(eacp-core PRIVATE
                "-framework Cocoa"
                "-framework Security"
                "-framework ServiceManagement"
                "-framework UniformTypeIdentifiers")
    endif ()
elseif (WIN32)
    target_sources(eacp-core PRIVATE
            App/App-Windows.cpp
            App/Clipboard-Windows.cpp
            App/LoginItem-Windows.cpp
            Plugins/DynamicLibrary-Windows.cpp
            Plugins/ModuleInfo-Windows.cpp
            Process/Process-Windows.cpp
            Threads/EventLoop-Windows.cpp
            Threads/ThreadUtils-Windows.cpp
            Threads/Timer-Windows.cpp
            Utils/Environment-Windows.cpp
            Utils/Files-Windows.cpp
            Utils/Logging-Windows.cpp)
    target_link_libraries(eacp-core PRIVATE windowsapp crypt32 user32 advapi32)
elseif (UNIX AND NOT APPLE)
    target_sources(eacp-core PRIVATE
            App/App-Linux.cpp
            App/Clipboard-Linux.cpp
            App/LoginItem-Linux.cpp
            Plugins/DynamicLibrary-Linux.cpp
            Plugins/DynamicLibrary-Posix.cpp
            Plugins/ModuleInfo-Linux.cpp
            Plugins/ModuleInfo-Posix.cpp
            Process/Process-Posix.cpp
            Threads/EventLoop-Linux.cpp
            Threads/EventLoop-Default.cpp
            Threads/ThreadUtils-Linux.cpp
            Threads/Timer-Linux.cpp
            Utils/Environment-Posix.cpp
            Utils/Files-Linux.cpp
            Utils/Logging-Posix.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(eacp-core PRIVATE Threads::Threads)
endif ()

set_default_target_setting(eacp-core)
eacp_enable_unity_build(eacp-core)
add_ide_sources(eacp-core)

add_subdirectory(Testing)
