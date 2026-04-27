include(FetchContent)

set(NANO_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(NanoTest
        GIT_REPOSITORY https://github.com/eyalamirmusic/NanoTest.git
        GIT_TAG main)

FetchContent_MakeAvailable(NanoTest)
