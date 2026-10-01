# The session daemon: a headless PTY server the GUI dials over eacp IPC.
# Shares the app's Pty and protocol sources rather than a library — three
# files does not justify one.
add_executable(CowTermDaemon Main.cpp)

if (APPLE)
    target_sources(CowTermDaemon PRIVATE ../Terminal/Pty-Posix.cpp)
elseif (WIN32)
    target_sources(CowTermDaemon PRIVATE ../Terminal/Pty-Windows.cpp)
    target_compile_options(CowTermDaemon PRIVATE /utf-8)
endif ()

target_include_directories(CowTermDaemon PRIVATE ../Terminal)
target_link_libraries(CowTermDaemon PRIVATE eacp-core eacp-network)

set_default_target_setting(CowTermDaemon)
