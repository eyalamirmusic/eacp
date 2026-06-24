# eacp_add_webview_test — define a NanoTest binary that drives a
# WebView app in-process. The test exe statically links the app
# (the ${APP}_app library produced by eacp_add_webview_app's library
# mode), pre-initializes NSApplication, and runs each test directly
# on the main thread.
#
# Usage:
#   eacp_add_webview_test(<TARGET>
#       APP     <app-target>          # the eacp_add_webview_app target
#                                     # (must have been built with
#                                     # APP_HEADER for library mode)
#       SOURCES <files>               # NanoTest source files
#   )
#
# The APP target must expose a STATIC `${APP}_app` companion (this
# is what library-mode eacp_add_webview_app produces). On
# unsupported platforms (iOS / Linux) eacp-webview-test is not
# built and this function is a no-op.
function(eacp_add_webview_test TARGET)
    set(oneValueArgs APP)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT ARG_APP)
        message(FATAL_ERROR
                "eacp_add_webview_test(${TARGET}): APP is required")
    endif ()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR
                "eacp_add_webview_test(${TARGET}): SOURCES is required")
    endif ()

    if (NOT TARGET eacp-webview-test)
        return ()
    endif ()
    if (NOT TARGET ${ARG_APP}_app)
        message(FATAL_ERROR
                "eacp_add_webview_test(${TARGET}): ${ARG_APP}_app target "
                "does not exist. The APP must be built with library mode "
                "(pass APP_HEADER to eacp_add_webview_app).")
    endif ()

    add_executable(${TARGET} ${ARG_SOURCES})

    # Group with the app's other targets in IDE target trees.
    get_target_property(APP_FOLDER ${ARG_APP} FOLDER)
    if (APP_FOLDER)
        set_target_properties(${TARGET} PROPERTIES FOLDER "${APP_FOLDER}")
    endif ()

    # Link the app's static lib (gets MyApp's header + all the
    # plumbing — schema, web resources, eacp-webview, etc.) plus the
    # test driver and prebuilt main.
    target_link_libraries(${TARGET} PRIVATE
            ${ARG_APP}_app
            eacp-webview-test
            eacp-webview-test-main)

    # CTest discovery uses NanoTest's --list-tests; reuses the
    # discover step shipped with NanoTest.
    nano_discover_tests(${TARGET})
endfunction()
