# eacp_webview_generate_backend — emit the TypeScript shim that binds
# Miro's transport-agnostic makeBridge runtime to the eacp WebView
# transport (window.eacp). The shim is rendered from
# WebView/Resources/EacpBackend.ts.template via CMake's configure_file —
# the only thing it carries that a configure-time substitution can't
# express is the schema basename, which is supplied by the caller.
#
# Usage:
#   eacp_webview_generate_backend(
#       OUTPUT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
#       BASENAME    schema                     # stem of schema files (default: schema)
#       [OUTPUT_NAME backend]                  # filename stem (default: backend)
#   )
#
# Side effect: writes ${OUTPUT_DIR}/${OUTPUT_NAME}.ts at configure time
# (and again on reconfigure if the template changes — CMake tracks it).
function(eacp_webview_generate_backend)
    cmake_parse_arguments(ARG ""
            "BASENAME;OUTPUT_DIR;OUTPUT_NAME" "" ${ARGN})

    if (NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR
                "eacp_webview_generate_backend: OUTPUT_DIR is required")
    endif ()
    if (NOT ARG_BASENAME)
        set(ARG_BASENAME "schema")
    endif ()
    if (NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "backend")
    endif ()

    # Render into the per-build-dir binary tree, then atomically publish into
    # OUTPUT_DIR (which lives in the shared source tree). See
    # eacp_webview_publish_generated for why a plain configure_file into the
    # source tree races when multiple build dirs configure at once.
    set(BASENAME "${ARG_BASENAME}")
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/eacp-webview-gen/${ARG_OUTPUT_NAME}.ts")
    # NEWLINE_STYLE LF: configure_file defaults to the platform-native newline
    # (CRLF on Windows), which would make the generated .ts drift from the
    # repo's eol=lf policy and show up perpetually dirty on Windows checkouts.
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpBackend.ts.template"
            "${staged}"
            @ONLY
            NEWLINE_STYLE LF)
    eacp_webview_publish_generated(
            "${staged}" "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts")
endfunction()

# eacp_webview_generate_mock — emit the dev/test loopback bridge that installs a
# window.eacp routing invoke() through the generated dispatch() and looping
# on()/expose() in-process. Lets the generated client (backend/hooks/react) run
# host-less — `npm run dev` in a browser, or a jsdom test — with the app
# supplying the scenario. Generic: like the backend shim, the only thing it
# carries that a configure-time substitution can't express is the schema
# basename. Rendered from WebView/Resources/EacpMock.ts.template.
#
# Usage mirrors eacp_webview_generate_backend:
#   eacp_webview_generate_mock(
#       OUTPUT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
#       BASENAME    schema                     # stem of schema files (default: schema)
#       [OUTPUT_NAME mock]                     # filename stem (default: mock)
#   )
#
# Emitted unconditionally alongside the backend; it is tree-shaken out of the
# production embed when no module imports it (only an app's dev scenario does).
function(eacp_webview_generate_mock)
    cmake_parse_arguments(ARG ""
            "BASENAME;OUTPUT_DIR;OUTPUT_NAME" "" ${ARGN})

    if (NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR
                "eacp_webview_generate_mock: OUTPUT_DIR is required")
    endif ()
    if (NOT ARG_BASENAME)
        set(ARG_BASENAME "schema")
    endif ()
    if (NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "mock")
    endif ()

    # Same staged-then-atomically-published flow as the backend shim (see
    # eacp_webview_publish_generated for why a direct configure_file into the
    # shared source tree races across concurrent build dirs).
    set(BASENAME "${ARG_BASENAME}")
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/eacp-webview-gen/${ARG_OUTPUT_NAME}.ts")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpMock.ts.template"
            "${staged}"
            @ONLY
            NEWLINE_STYLE LF)
    eacp_webview_publish_generated(
            "${staged}" "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts")
endfunction()
