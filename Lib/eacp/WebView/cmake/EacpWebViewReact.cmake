# eacp_webview_generate_react_hooks — emit the React hooks module that
# pairs with the typed `backend` client. Rendered from a template via
# CMake's configure_file (@ONLY) so the import of TransportOn can be
# pointed at the matching <basename>.bridge module — TS unifies generic
# inference across the bridge and the hooks only when they share the
# exact same TransportOn symbol, not just structurally equivalent ones.
#
# Usage:
#   eacp_webview_generate_react_hooks(
#       OUTPUT_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
#       [BASENAME    schema]                   # bridge module stem (default: schema)
#       [OUTPUT_NAME react]                    # filename stem (default: react)
#   )
function(eacp_webview_generate_react_hooks)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR;OUTPUT_NAME;BASENAME" "" ${ARGN})

    if (NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR
                "eacp_webview_generate_react_hooks: OUTPUT_DIR is required")
    endif ()
    if (NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "react")
    endif ()
    if (NOT ARG_BASENAME)
        set(ARG_BASENAME "schema")
    endif ()

    set(BASENAME "${ARG_BASENAME}")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpReact.ts.template"
            "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts"
            @ONLY)
endfunction()
