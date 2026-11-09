# Configure-time checks of CMake/AppleSetup.cmake: the generated Xcode
# project signs with the development team the project names, and with eacp's
# only when it names none. Each scenario configures a small fixture with the
# Xcode generator for iOS, so this runs only where Xcode is installed.
find_program(EACP_XCODEBUILD xcodebuild)

if (NOT EACP_XCODEBUILD)
    return()
endif ()

add_test(NAME AppleSetup/DevelopmentTeam
        COMMAND "${CMAKE_COMMAND}"
        "-DEACP_CMAKE_DIR=${PROJECT_SOURCE_DIR}/CMake"
        "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Fixture"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/DevelopmentTeam"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckDevelopmentTeam.cmake")
