# eacp_webview_generate_backend — writes a self-contained TypeScript
# shim that binds Miro's transport-agnostic makeBackend factory to
# window.eacp.invoke. Replaces the boilerplate every embedded webview
# project would otherwise hand-write — Miro stays transport-agnostic
# and this is the only place that knows about window.eacp.
#
# Usage:
#   eacp_webview_generate_backend(
#       OUTPUT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/web/src/generated
#       BASENAME    schema     # stem of the schema files (default: schema)
#       OUTPUT_NAME backend    # filename stem (default: backend)
#   )
#
# Side effect: writes ${OUTPUT_DIR}/${OUTPUT_NAME}.ts at configure time.
# The file imports makeBackend from "./${BASENAME}.backend" and binds
# it to window.eacp.invoke. Re-runs every configure so template changes
# in this file propagate without an explicit clean.
function(eacp_webview_generate_backend)
    cmake_parse_arguments(ARG "" "BASENAME;OUTPUT_DIR;OUTPUT_NAME" "" ${ARGN})

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

    set(SHIM_PATH "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.ts")

    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
    file(WRITE "${SHIM_PATH}"
"import { makeBackend } from './${ARG_BASENAME}.backend';

interface EacpBridge
{
    invoke<Res = unknown, Req = unknown>(command: string, payload?: Req): Promise<Res>;
    on<T = unknown>(event: string, handler: (payload: T) => void): () => void;
}

declare global
{
    interface Window
    {
        eacp: EacpBridge;
    }
}

export const backend = makeBackend((command, payload) =>
    window.eacp.invoke(command, payload));
")
endfunction()
