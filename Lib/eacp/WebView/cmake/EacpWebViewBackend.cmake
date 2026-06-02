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
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpBackend.ts.template"
            "${staged}"
            @ONLY)
    eacp_webview_publish_generated(
            "${staged}" "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts")
endfunction()
