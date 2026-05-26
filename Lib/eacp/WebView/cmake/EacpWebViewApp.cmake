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
#       [TEST_HOST_SOURCES <files>]       # if set, also produces
#                                         # ${TARGET}TestHost — a sibling
#                                         # executable sharing the schema +
#                                         # web bundle, linking eacp-webview-
#                                         # test so an external runner can
#                                         # drive the WebView over /rpc.
#       [TESTS_DIR <abs path>]            # if set (requires TEST_HOST_SOURCES),
#                                         # registers a CTest test named
#                                         # ${TARGET}Tests that drives the
#                                         # test host via Playwright. The
#                                         # spec dir is materialized as a
#                                         # self-contained npm project (see
#                                         # EacpWebViewNodeTests.cmake).
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
    set(oneValueArgs WEB_DIR BUNDLE_ID BUNDLE_NAME NAMESPACE CATEGORY SCHEMA_NAME PACKAGE_MANAGER TESTS_DIR)
    set(multiValueArgs SOURCES COMMAND_SOURCES SCHEMA_FORMATS API API_HEADER TEST_HOST_SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (ARG_TESTS_DIR AND NOT ARG_TEST_HOST_SOURCES)
        message(FATAL_ERROR
                "eacp_add_webview_app(${TARGET}): TESTS_DIR requires "
                "TEST_HOST_SOURCES (the test runner drives the test host).")
    endif ()

    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "eacp_add_webview_app(${TARGET}): SOURCES is required")
    endif ()
    if (ARG_COMMAND_SOURCES AND ARG_API)
        message(FATAL_ERROR
                "eacp_add_webview_app(${TARGET}): COMMAND_SOURCES and API "
                "are mutually exclusive")
    endif ()
    if (ARG_API AND NOT ARG_API_HEADER)
        message(FATAL_ERROR
                "eacp_add_webview_app(${TARGET}): API_HEADER is required "
                "when API is set")
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
        if (ARG_REACT)
            list(APPEND ARG_SCHEMA_FORMATS hooks)
        endif ()
    endif ()

    set(TS_GENERATED_DIR "${ARG_WEB_DIR}/src/generated")
    set(CPP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/cpp-generated")

    add_executable(${TARGET} ${ARG_SOURCES})
    target_link_libraries(${TARGET} PRIVATE eacp-webview eacp-network-rpc)

    if (ARG_TEST_HOST_SOURCES)
        add_executable(${TARGET}TestHost ${ARG_TEST_HOST_SOURCES})
        target_link_libraries(${TARGET}TestHost PRIVATE
                eacp-webview eacp-network-rpc eacp-webview-test)
    endif ()

    if (ARG_COMMAND_SOURCES OR ARG_API)
        # Two schema modes, picked by which arg the caller supplied.
        # SOURCES paths in miro_export are resolved against
        # CMAKE_CURRENT_SOURCE_DIR at *call* site, which inside this
        # function is the caller's directory — so bare filenames work
        # for either mode.
        if (ARG_API)
            miro_export(${TARGET}Schema
                    API ${ARG_API}
                    API_HEADER ${ARG_API_HEADER}
                    OUTPUT_DIR ${TS_GENERATED_DIR}
                    OUTPUT_NAME ${ARG_SCHEMA_NAME}
                    FORMATS ${ARG_SCHEMA_FORMATS})
        else ()
            miro_export(${TARGET}Schema
                    SOURCES ${ARG_COMMAND_SOURCES}
                    OUTPUT_DIR ${TS_GENERATED_DIR}
                    OUTPUT_NAME ${ARG_SCHEMA_NAME}
                    FORMATS ${ARG_SCHEMA_FORMATS})
        endif ()

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

        # SOURCES mode needs HANDLERS splicing (so MIRO_EXPORT_COMMAND
        # static initializers fire in the runtime executable). API mode
        # binds at runtime via bridge.use(api), so it only needs the
        # generated headers — plain link, no source splicing.
        if (ARG_COMMAND_SOURCES)
            eacp_target_uses_schema(${TARGET} ${TARGET}Schema HANDLERS)
            if (ARG_TEST_HOST_SOURCES)
                eacp_target_uses_schema(${TARGET}TestHost ${TARGET}Schema HANDLERS)
            endif ()
        else ()
            eacp_target_uses_schema(${TARGET} ${TARGET}Schema)
            if (ARG_TEST_HOST_SOURCES)
                eacp_target_uses_schema(${TARGET}TestHost ${TARGET}Schema)
            endif ()
        endif ()

        # Schema codegen needs eacp's events / hooks formatter
        # registrations to be alive. Linking the eacp-webview-codegen
        # OBJECT lib directly into the codegen executable splices its
        # objects in and keeps the static-init constructors from being
        # dropped by the linker.
        if (TARGET ${TARGET}Schema_Codegen)
            target_link_libraries(${TARGET}Schema_Codegen PRIVATE eacp-core)
            if (TARGET eacp-webview-codegen)
                target_link_libraries(${TARGET}Schema_Codegen PRIVATE
                        eacp-webview-codegen)
            endif ()
        endif ()
    endif ()

    if (ARG_REACT)
        eacp_webview_generate_react_hooks(
                OUTPUT_DIR ${TS_GENERATED_DIR}
                BASENAME ${ARG_SCHEMA_NAME})
    endif ()

    # PACKAGE_MANAGER (optional) — forwarded so callers can opt into pnpm/
    # yarn/bun for the embedded vite build without flipping the global
    # EACP_WEBVIEW_PACKAGE_MANAGER cache var. When unset, eacp_webview_add_vite
    # falls back to its own default (the cache var, defaulting to npm).
    set(VITE_ARGS
            SOURCE_DIR ${ARG_WEB_DIR}
            NAMESPACE ${ARG_NAMESPACE}
            CATEGORY ${ARG_CATEGORY})
    if (ARG_PACKAGE_MANAGER)
        list(APPEND VITE_ARGS PACKAGE_MANAGER ${ARG_PACKAGE_MANAGER})
    endif ()

    # Vite imports the codegen'd TS modules, so the build-time vite step
    # must run after the schema's codegen exec emits them. Passing the
    # codegen target through DEPENDS gives both the ordering edge and a
    # rebuild edge — vite re-runs whenever the codegen exe relinks (i.e.
    # whenever a COMMAND_SOURCES file changes).
    if (TARGET ${TARGET}Schema_Codegen)
        list(APPEND VITE_ARGS DEPENDS ${TARGET}Schema_Codegen)
    endif ()

    eacp_webview_add_vite(${TARGET} ${VITE_ARGS})

    set_target_properties(${TARGET} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${ARG_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER ${ARG_BUNDLE_ID}
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${ARG_BUNDLE_ID})

    set_default_target_setting(${TARGET})

    if (ARG_TEST_HOST_SOURCES)
        eacp_webview_add_vite(${TARGET}TestHost ${VITE_ARGS})

        set_target_properties(${TARGET}TestHost PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_BUNDLE_NAME "${ARG_BUNDLE_NAME} TestHost"
                MACOSX_BUNDLE_GUI_IDENTIFIER ${ARG_BUNDLE_ID}.testhost
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${ARG_BUNDLE_ID}.testhost)

        set_default_target_setting(${TARGET}TestHost)
    endif ()

    if (ARG_TESTS_DIR)
        _eacp_register_webview_node_tests(
                TEST_NAME ${TARGET}Tests
                TEST_HOST ${TARGET}TestHost
                TESTS_DIR ${ARG_TESTS_DIR}
                PACKAGE_MANAGER ${ARG_PACKAGE_MANAGER})
    endif ()
endfunction()
