# A project that uses eacp's Apple setup the way a consumer would, configured
# by CheckDevelopmentTeam.cmake once per scenario. The FIXTURE_* variables pick
# where the project names its own development team relative to the setup:
# before it or after it, as a normal variable or a cache entry, and whether
# the setup runs in the top-level directory or in a subdirectory.
cmake_minimum_required(VERSION 3.31)

project(EacpAppleSetupFixture LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${EACP_CMAKE_DIR}")
include(TargetSetup)

if (DEFINED FIXTURE_NORMAL_BEFORE)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FIXTURE_NORMAL_BEFORE}")
endif ()

if (FIXTURE_SETUP_IN_SUBDIR)
    add_subdirectory(Sub)
else ()
    eacp_default_setup()
endif ()

if (DEFINED FIXTURE_NORMAL_AFTER)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FIXTURE_NORMAL_AFTER}")
endif ()

if (DEFINED FIXTURE_CACHE_AFTER)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FIXTURE_CACHE_AFTER}"
            CACHE STRING "The consumer's development team")
endif ()

add_custom_target(FixtureTarget)
