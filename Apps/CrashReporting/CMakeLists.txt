set(CMAKE_FOLDER Examples/CrashReporting)

# Two buttons per way an app can die, one faulting the main thread and one a
# background thread, from eacp-crash-faults's list (Lib/eacp/CrashReporting/Faults),
# and the report the handler process writes and shows in a window of its own.
# `SelfDestruct --crash <kind> --thread main|background` does it with no
# window, which is what Tests/CrashReporting runs.
add_subdirectory(SelfDestruct)
