# The session daemon: a headless PTY server the GUI dials over eacp IPC.
# Shares the app's Pty and protocol sources rather than a library — three
# files does not justify one.
add_executable(TerminalDaemon
        Main.cpp
        ../Terminal/Pty-Posix.cpp)

target_include_directories(TerminalDaemon PRIVATE ../Terminal)
target_link_libraries(TerminalDaemon PRIVATE eacp-core eacp-network)

set_default_target_setting(TerminalDaemon)
