set(CMAKE_FOLDER Examples/CrashReporting)

# A button per way an app can die -- a null dereference, a stack overflow,
# abort(), a pure virtual call, an exception nobody catches, heap corruption,
# and __fastfail on Windows -- each faulting the app's own process, and the
# report its handler process writes and shows in a window of its own.
# `SelfDestruct --crash <kind>` does it with no window, which is what
# Tests/CrashReporting runs.
add_subdirectory(SelfDestruct)
