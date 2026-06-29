set(EACP_APPHUB_BUNDLE_ID "com.tamber.AppHub" CACHE STRING "AppHub bundle identifier")
set(EACP_APPHUB_HELPER_LABEL
    "com.tamber.AppHub.PrivilegedHelper"
    CACHE STRING
    "AppHub privileged helper launchd label")
set(EACP_APPHUB_APP_SIGNING_REQUIREMENT
    "identifier \"${EACP_APPHUB_BUNDLE_ID}\" and anchor apple generic"
    CACHE STRING
    "Code signing requirement for the AppHub application")
set(EACP_APPHUB_HELPER_SIGNING_REQUIREMENT
    "identifier \"${EACP_APPHUB_HELPER_LABEL}\" and anchor apple generic"
    CACHE STRING
    "Code signing requirement for the AppHub privileged helper")

if (APPLE)
    add_executable(AppHub MACOSX_BUNDLE Main.cpp PrivilegedHelperClient-macOS.mm)
else ()
    add_executable(AppHub Main.cpp PrivilegedHelperClient-Unsupported.cpp)
endif ()

target_link_libraries(AppHub PRIVATE eacp-updater)

set_default_target_setting(AppHub)

target_compile_definitions(AppHub
    PRIVATE
        EACP_APPHUB_HELPER_LABEL="${EACP_APPHUB_HELPER_LABEL}"
)

if (APPLE)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AppHubInfo.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AppHubInfo.plist"
        @ONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/PrivilegedHelper/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PrivilegedHelperInfo.plist"
        @ONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/PrivilegedHelper/LaunchDaemon.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${EACP_APPHUB_HELPER_LABEL}.plist"
        @ONLY)

    set_target_properties(AppHub
        PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/AppHubInfo.plist"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${EACP_APPHUB_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_NAME "Tamber App Hub"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
            MACOSX_BUNDLE_BUNDLE_VERSION "1")

    add_executable(AppHubPrivilegedHelper PrivilegedHelper/Main.cpp)
    target_link_libraries(AppHubPrivilegedHelper PRIVATE eacp-updater)
    set_default_target_setting(AppHubPrivilegedHelper)
    set_target_properties(AppHubPrivilegedHelper
        PROPERTIES
            OUTPUT_NAME "${EACP_APPHUB_HELPER_LABEL}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/PrivilegedHelperInfo.plist")
    target_compile_definitions(AppHubPrivilegedHelper
        PRIVATE
            EACP_APPHUB_HELPER_LABEL="${EACP_APPHUB_HELPER_LABEL}")
    target_link_options(AppHubPrivilegedHelper
        PRIVATE
            "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/PrivilegedHelperInfo.plist"
            "-Wl,-sectcreate,__TEXT,__launchd_plist,${CMAKE_CURRENT_BINARY_DIR}/${EACP_APPHUB_HELPER_LABEL}.plist")

    add_dependencies(AppHub AppHubPrivilegedHelper)

    add_custom_command(TARGET AppHub POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:AppHub>/Contents/Library/LaunchServices"
        COMMAND "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:AppHubPrivilegedHelper>"
                "$<TARGET_BUNDLE_DIR:AppHub>/Contents/Library/LaunchServices/${EACP_APPHUB_HELPER_LABEL}"
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/${EACP_APPHUB_HELPER_LABEL}.plist"
                "$<TARGET_BUNDLE_DIR:AppHub>/Contents/Library/LaunchServices/${EACP_APPHUB_HELPER_LABEL}.plist"
        VERBATIM)
endif ()
