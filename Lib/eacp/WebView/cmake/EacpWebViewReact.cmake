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

    # Render into the per-build-dir binary tree, then atomically publish into
    # OUTPUT_DIR (shared source tree) — see eacp_webview_publish_generated for
    # the concurrent-configure race a plain configure_file would hit here.
    set(BASENAME "${ARG_BASENAME}")
    set(stagedReact "${CMAKE_CURRENT_BINARY_DIR}/eacp-webview-gen/${ARG_OUTPUT_NAME}.ts")
    # NEWLINE_STYLE LF: configure_file defaults to the platform-native newline
    # (CRLF on Windows), which would make the generated .ts drift from the
    # repo's eol=lf policy and show up perpetually dirty on Windows checkouts.
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpReact.ts.template"
            "${stagedReact}"
            @ONLY
            NEWLINE_STYLE LF)
    eacp_webview_publish_generated(
            "${stagedReact}" "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts")

    # Also emit a stable-named hooks.ts re-export so user code can
    # `import { useTodos } from './generated/hooks'` regardless of the
    # schema basename. The underlying `<basename>.hooks.ts` is the
    # Miro-emitted hooks module; this shim just renames the import path.
    set(stagedHooks "${CMAKE_CURRENT_BINARY_DIR}/eacp-webview-gen/hooks.ts")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../WebView/Resources/EacpHooks.ts.template"
            "${stagedHooks}"
            @ONLY
            NEWLINE_STYLE LF)
    eacp_webview_publish_generated("${stagedHooks}" "${ARG_OUTPUT_DIR}/hooks.ts")
endfunction()
