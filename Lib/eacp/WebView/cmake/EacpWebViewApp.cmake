# eacp_add_webview_app — single entry point for native + WebView + RPC apps.
#
# Folds the whole pipeline (executable, schema library + codegen, vite
# build, ResEmbed, macOS bundle, transport binding) into one call.
# Apps that don't need any cross-app glue should require nothing more
# than this one function call in their CMakeLists.txt.
#
# Usage:
#   eacp_add_webview_app(<TARGET>
#       SOURCES         <files>           # main() + non-RPC sources
#       COMMAND_SOURCES <files>           # files with MIRO_EXPORT_COMMAND;
#                                         # compiled into ${TARGET}Schema
#       WEB_DIR         <path>            # vite project dir (must contain package.json)
#       BUNDLE_ID       <com.example.app>
#       BUNDLE_NAME     "Display name"
#       [NAMESPACE      <ns>]             # res-embed namespace; default WebResources
#       [CATEGORY       <cat>]            # res-embed category; default WebApp
#       [SCHEMA_NAME    <stem>]           # default: schema
#       [SCHEMA_FORMATS <formats>]        # default: ts backend ts-server bridge
#                                         # (cpp-miro and cpp-client are always
#                                         # emitted so siblings can consume the
#                                         # schema via eacp_target_uses_schema)
#       [REACT]                           # emit React hook bindings into
#                                         # ${WEB_DIR}/src/generated/react.ts
#   )
#
# Schema layout:
#   - ${TARGET}Schema is the INTERFACE library produced by miro_export.
#     It carries the generated-header include dirs and the registration
#     source list. eacp_target_uses_schema(<consumer> ${TARGET}Schema
#     HANDLERS) splices those sources into the consumer so the
#     MIRO_EXPORT_COMMAND static initializers fire at startup.
#   - Build-time codegen emits TS files into ${WEB_DIR}/src/generated
#     and C++ headers into ${CMAKE_CURRENT_BINARY_DIR}/cpp-generated.
#     Both directories ride along on the schema's INTERFACE includes,
#     so any consumer that links the schema picks them up automatically.
function(eacp_add_webview_app TARGET)
    set(options REACT)
    set(oneValueArgs WEB_DIR BUNDLE_ID BUNDLE_NAME NAMESPACE CATEGORY SCHEMA_NAME)
    set(multiValueArgs SOURCES COMMAND_SOURCES SCHEMA_FORMATS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "eacp_add_webview_app(${TARGET}): SOURCES is required")
    endif ()
    if (NOT ARG_WEB_DIR)
        message(FATAL_ERROR "eacp_add_webview_app(${TARGET}): WEB_DIR is required")
    endif ()
    if (NOT ARG_BUNDLE_ID)
        message(FATAL_ERROR "eacp_add_webview_app(${TARGET}): BUNDLE_ID is required")
    endif ()
    if (NOT ARG_BUNDLE_NAME)
        message(FATAL_ERROR "eacp_add_webview_app(${TARGET}): BUNDLE_NAME is required")
    endif ()
    if (NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "WebResources")
    endif ()
    if (NOT ARG_CATEGORY)
        set(ARG_CATEGORY "WebApp")
    endif ()
    if (NOT ARG_SCHEMA_NAME)
        set(ARG_SCHEMA_NAME "schema")
    endif ()
    if (NOT ARG_SCHEMA_FORMATS)
        set(ARG_SCHEMA_FORMATS ts backend ts-server bridge events)
    endif ()

    set(TS_GENERATED_DIR "${ARG_WEB_DIR}/src/generated")
    set(CPP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cpp-generated")

    add_executable(${TARGET} ${ARG_SOURCES})
    target_link_libraries(${TARGET} PRIVATE eacp-webview eacp-network-rpc)

    if (ARG_COMMAND_SOURCES)
        # SOURCES paths in miro_export are resolved against
        # CMAKE_CURRENT_SOURCE_DIR at *call* site, which inside this
        # function is the caller's directory — so bare filenames work.
        miro_export(${TARGET}Schema
                SOURCES ${ARG_COMMAND_SOURCES}
                OUTPUT_DIR ${TS_GENERATED_DIR}
                OUTPUT_NAME ${ARG_SCHEMA_NAME}
                FORMATS ${ARG_SCHEMA_FORMATS})

        # Always also emit the C++ artifacts so any sibling target can
        # consume them via eacp_target_uses_schema without having to
        # tack on its own miro_export_emit().
        miro_export_emit(${TARGET}Schema
                OUTPUT_DIR ${CPP_GENERATED_DIR}
                OUTPUT_NAME ${ARG_SCHEMA_NAME}
                FORMATS cpp-miro cpp-client)

        eacp_webview_generate_backend(
                OUTPUT_DIR ${TS_GENERATED_DIR}
                BASENAME ${ARG_SCHEMA_NAME})

        eacp_target_uses_schema(${TARGET} ${TARGET}Schema HANDLERS)

        # Command sources may reach into eacp-core types (e.g.
        # StateValue<T> declared in the app's Types.h). The codegen tool
        # never *calls* runtime code, but it has to compile and link the
        # same TU, so pull in eacp-core's include path and static lib.
        if (TARGET ${TARGET}Schema_Codegen)
            target_link_libraries(${TARGET}Schema_Codegen PRIVATE eacp-core)
        endif ()
    endif ()

    if (ARG_REACT)
        eacp_webview_generate_react_hooks(
                OUTPUT_DIR ${TS_GENERATED_DIR}
                BASENAME ${ARG_SCHEMA_NAME})
    endif ()

    eacp_webview_add_vite(${TARGET}
            SOURCE_DIR ${ARG_WEB_DIR}
            NAMESPACE ${ARG_NAMESPACE}
            CATEGORY ${ARG_CATEGORY})

    # Vite imports the codegen'd TS modules, so the build-time vite step
    # must run after the schema's codegen exec emits them.
    if (TARGET ${TARGET}-vite-build AND TARGET ${TARGET}Schema_Codegen)
        add_dependencies(${TARGET}-vite-build ${TARGET}Schema_Codegen)
    endif ()

    set_target_properties(${TARGET} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${ARG_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER ${ARG_BUNDLE_ID}
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${ARG_BUNDLE_ID})

    set_default_target_setting(${TARGET})
endfunction()
