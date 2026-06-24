# eacp_add_webview_app — single entry point for native + WebView + RPC apps.
#
# Folds the whole pipeline (app library, executable, schema library +
# codegen, vite build, ResEmbed, macOS bundle) into one call.
#
# Two modes, picked by whether APP_HEADER is passed:
#
#  - LEGACY: caller passes SOURCES <Main.cpp ...> where the file
#    contains both MyApp's struct AND main(). The helper builds a
#    single executable target. No companion library; the app cannot
#    be driven from tests because the type isn't visible outside the
#    executable's TUs.
#
#  - LIBRARY: caller additionally passes APP_HEADER <App.h>. MyApp
#    lives in App.h with `int main()` factored into the (still
#    user-supplied) SOURCES files. The helper builds a STATIC
#    ${TARGET}_app library carrying the app's plumbing (schema, vite
#    resources, link deps) and an executable that links it. Tests
#    link ${TARGET}_app via eacp_add_webview_test().
#
# Usage (library mode):
#   eacp_add_webview_app(<TARGET>
#       APP_HEADER      <header>          # header containing the app struct
#       SOURCES         <files>           # at least Main.cpp (no MyApp body)
#       [COMMAND_SOURCES <files>]         # MIRO_EXPORT_COMMAND TUs
#       WEB_DIR         <path>
#       BUNDLE_ID       <com.example.app>
#       BUNDLE_NAME     "Display name"
#       [NAMESPACE      <ns>]             # res-embed namespace; default WebResources
#       [CATEGORY       <cat>]            # res-embed category; default WebApp
#       [SCHEMA_NAME    <stem>]           # default: schema
#       [SCHEMA_FORMATS <formats>]        # default: ts backend ts-server bridge
#       [API            <type>]           # reflectable API class to bind
#       [API_HEADER     <header>]         # header for API (required with API)
#       [LINK_LIBRARIES <libs>]           # extra link deps for the app AND the
#                                         # schema codegen tool (see note below)
#       [REACT]                           # emit React hook bindings
#   )
#
# LINK_LIBRARIES is for native libraries the app's API/command headers
# reference (e.g. a TamberLib carrying CLAP types). They are linked onto the
# app target (PUBLIC on the static lib in library mode so the exe + tests
# inherit them, PRIVATE on the exe in legacy mode) AND onto the schema codegen
# tool. The codegen tool default-constructs each API to walk reflect(), so it
# must both see those headers and link whatever symbols default construction /
# destruction ODR-uses — passing the library here covers include dirs and link
# in one shot, so callers don't have to reach into the generated
# ${TARGET}Schema_Codegen target by hand.
function(eacp_add_webview_app TARGET)
    set(options REACT)
    set(oneValueArgs WEB_DIR BUNDLE_ID BUNDLE_NAME NAMESPACE CATEGORY SCHEMA_NAME
            PACKAGE_MANAGER APP_HEADER)
    set(multiValueArgs SOURCES COMMAND_SOURCES SCHEMA_FORMATS API API_HEADER
            LINK_LIBRARIES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

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

    # Library mode adds a STATIC ${TARGET}_app carrying everything the
    # test target needs to link. The executable links it for the
    # plumbing; its sources are the user-supplied Main.cpp (+ any
    # other exe-only files). The static lib gets no user sources by
    # default — App.h ships through INTERFACE include propagation,
    # not compilation.
    if (ARG_APP_HEADER)
        set(APP_LIB_TARGET "${TARGET}_app")
        add_library(${APP_LIB_TARGET} STATIC)
        target_link_libraries(${APP_LIB_TARGET} PUBLIC
                eacp-webview eacp-network-rpc)
        target_include_directories(${APP_LIB_TARGET} PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR})

        # APP_HEADER as a HEADER_FILE_ONLY source for IDE visibility.
        target_sources(${APP_LIB_TARGET} PRIVATE ${ARG_APP_HEADER})
        set_source_files_properties(${ARG_APP_HEADER} PROPERTIES
                HEADER_FILE_ONLY TRUE)

        add_executable(${TARGET} ${ARG_SOURCES})
        target_link_libraries(${TARGET} PRIVATE ${APP_LIB_TARGET})
    else ()
        # Legacy single-executable layout.
        set(APP_LIB_TARGET "${TARGET}")
        add_executable(${TARGET} ${ARG_SOURCES})
        target_link_libraries(${TARGET} PRIVATE eacp-webview eacp-network-rpc)
    endif ()

    # Caller-supplied native link deps. PUBLIC on the static lib in library
    # mode (so the exe + test target inherit them, mirroring eacp-webview
    # above), PRIVATE on the exe in legacy mode. Also linked onto the codegen
    # tool further down.
    if (ARG_LINK_LIBRARIES)
        if (ARG_APP_HEADER)
            target_link_libraries(${APP_LIB_TARGET} PUBLIC ${ARG_LINK_LIBRARIES})
        else ()
            target_link_libraries(${APP_LIB_TARGET} PRIVATE ${ARG_LINK_LIBRARIES})
        endif ()
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
        # static initializers fire). API mode binds at runtime via
        # bridge.use(api), so only the generated headers are needed.
        if (ARG_COMMAND_SOURCES)
            eacp_target_uses_schema(${APP_LIB_TARGET} ${TARGET}Schema HANDLERS)
        else ()
            eacp_target_uses_schema(${APP_LIB_TARGET} ${TARGET}Schema)
        endif ()
        # eacp_target_uses_schema links PRIVATE. In library mode the
        # schema's include dirs need to propagate to consumers (the
        # exe + test exe), so re-link PUBLIC on the static lib.
        if (ARG_APP_HEADER)
            target_link_libraries(${APP_LIB_TARGET} PUBLIC ${TARGET}Schema)
        endif ()

        # Schema codegen needs eacp's hooks formatter registration to
        # be alive. Linking the eacp-webview-codegen OBJECT lib directly
        # into the codegen executable splices its objects in and keeps
        # the static-init constructors from being dropped by the
        # linker.
        if (TARGET ${TARGET}Schema_Codegen)
            target_link_libraries(${TARGET}Schema_Codegen PRIVATE eacp-core)
            if (TARGET eacp-webview-codegen)
                target_link_libraries(${TARGET}Schema_Codegen PRIVATE
                        eacp-webview-codegen)
            endif ()
            # The codegen stub #includes the API header and default-constructs
            # each API; give it the caller's native libs so those headers
            # resolve and the reflect() walk links.
            if (ARG_LINK_LIBRARIES)
                target_link_libraries(${TARGET}Schema_Codegen PRIVATE
                        ${ARG_LINK_LIBRARIES})
            endif ()

            # This is a build-time host tool a PostBuild script runs immediately
            # after linking. Under the Xcode generator on Apple Silicon, Xcode
            # passes the linker -no_adhoc_codesign (it expects to sign in a later
            # CodeSign phase), so the tool runs unsigned and the kernel kills it
            # (SIGKILL) on exec — arm64 binaries must be signed. Disabling signing
            # lets the linker apply its default ad-hoc signature so the tool runs.
            # (XCODE_ATTRIBUTE_* is ignored by the Ninja/Makefiles generators.)
            set_target_properties(${TARGET}Schema_Codegen PROPERTIES
                    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
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

    # Embed resources on the static lib in library mode (tests inherit
    # them), or on the executable in legacy mode.
    eacp_webview_add_vite(${APP_LIB_TARGET} ${VITE_ARGS})

    set_target_properties(${TARGET} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${ARG_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER ${ARG_BUNDLE_ID}
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${ARG_BUNDLE_ID})

    # A WebView app owns a window, never a console — the Windows counterpart to
    # MACOSX_BUNDLE above. No-op on non-Windows.
    eacp_set_gui_subsystem(${TARGET})

    set_default_target_setting(${TARGET})
    if (ARG_APP_HEADER)
        set_default_target_setting(${APP_LIB_TARGET})
    endif ()
endfunction()
