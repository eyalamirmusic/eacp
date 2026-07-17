option(EACP_WEBVIEW_VITE_BUILD "Let CMake drive '<pm> install' + 'vite build' for embedded webview apps. When OFF, the prebuilt dist committed at SOURCE_DIR/dist is embedded as-is." ON)

option(EACP_WEBVIEW_DEV "Dev mode: skip the vite production build and resource embedding entirely. Apps serve their UI from the vite dev server (run '<pm> run dev' in the app's web dir); the runtime already prefers a reachable dev server over embedded resources." OFF)

set(EACP_WEBVIEW_PACKAGE_MANAGER "npm" CACHE STRING
        "Package manager used to drive embedded vite builds (e.g. npm, pnpm, yarn, bun). Can be overridden per-call via the PACKAGE_MANAGER argument to eacp_webview_add_vite.")

function(eacp_webview_add_vite TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
            "SOURCE_DIR;DIST_DIR;NAMESPACE;CATEGORY;PACKAGE_MANAGER" "DEPENDS")

    if (NOT ARG_PACKAGE_MANAGER)
        set(ARG_PACKAGE_MANAGER "${EACP_WEBVIEW_PACKAGE_MANAGER}")
    endif ()

    if (NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "eacp_webview_add_vite: SOURCE_DIR is required")
    endif ()
    if (NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "Resources")
    endif ()
    if (NOT ARG_CATEGORY)
        set(ARG_CATEGORY "Resources")
    endif ()
    if (NOT ARG_DIST_DIR)
        set(ARG_DIST_DIR "${ARG_SOURCE_DIR}/dist")
    endif ()

    # Dev mode: no production bundle, no embed. Schema codegen still runs
    # (the app target depends on it directly), so the dev server picks up
    # fresh generated TS. Without a running dev server the app has no UI
    # to load — that's the expected trade-off of this mode.
    if (EACP_WEBVIEW_DEV)
        message(STATUS
                "eacp_webview_add_vite(${TARGET}): EACP_WEBVIEW_DEV is ON — "
                "skipping vite build + resource embed; serve the UI with the "
                "vite dev server from ${ARG_SOURCE_DIR}")
        return()
    endif ()

    if (EACP_WEBVIEW_VITE_BUILD AND EXISTS "${ARG_SOURCE_DIR}/package.json")
        find_program(EACP_WEBVIEW_PM_EXECUTABLE_${ARG_PACKAGE_MANAGER}
                NAMES ${ARG_PACKAGE_MANAGER}.cmd ${ARG_PACKAGE_MANAGER}
                REQUIRED)
        set(PM_EXECUTABLE "${EACP_WEBVIEW_PM_EXECUTABLE_${ARG_PACKAGE_MANAGER}}")

        get_filename_component(PM_DIR "${PM_EXECUTABLE}" DIRECTORY)
        find_program(EACP_VITE_NODE node REQUIRED)
        get_filename_component(NODE_DIR "${EACP_VITE_NODE}" DIRECTORY)
        unset(EACP_VITE_NODE CACHE)
        # Keyed on TARGET + NAMESPACE (the same pair res_embed_add keys its
        # generated dir on), so one target can embed several vite apps — e.g. an
        # app shell plus a plugin's editor page — without the dist dirs or
        # stamps colliding.
        set(BUILD_DIST_DIR
                "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-${ARG_NAMESPACE}-vite-dist")

        if (NOT EXISTS "${ARG_SOURCE_DIR}/node_modules")
            message(STATUS
                    "eacp_webview_add_vite(${TARGET}): running ${ARG_PACKAGE_MANAGER} install")
            execute_process(
                    COMMAND ${PM_EXECUTABLE} install
                    WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
                    RESULT_VARIABLE PM_INSTALL_RESULT)
            if (NOT PM_INSTALL_RESULT EQUAL 0)
                message(FATAL_ERROR
                        "${ARG_PACKAGE_MANAGER} install failed for ${TARGET}")
            endif ()
        endif ()

        file(MAKE_DIRECTORY "${BUILD_DIST_DIR}")

        # Glob over user-editable web sources so vite re-runs when one
        # changes. Generated TS files (codegen output under src/generated)
        # are also picked up here — they exist on disk after the first
        # build, and CONFIGURE_DEPENDS reglobs on subsequent invocations.
        # The actual ordering edge to the codegen target is provided via
        # ARG_DEPENDS below, so vite is correctly sequenced even on the
        # very first build when the generated files don't yet exist.
        file(GLOB_RECURSE VITE_SOURCES CONFIGURE_DEPENDS
                "${ARG_SOURCE_DIR}/src/*"
                "${ARG_SOURCE_DIR}/public/*"
                "${ARG_SOURCE_DIR}/index.html"
                "${ARG_SOURCE_DIR}/package.json"
                "${ARG_SOURCE_DIR}/vite.config.*"
                "${ARG_SOURCE_DIR}/tsconfig*.json")

        # npm requires `--` to forward args past its own CLI parser to the
        # underlying script. pnpm passes `--` through literally, which breaks
        # downstream tools (vite stops parsing at `--`). yarn / bun forward
        # args directly without a separator. Emit `--` only for npm.
        set(BUILD_CMD ${PM_EXECUTABLE} run build)
        if (ARG_PACKAGE_MANAGER STREQUAL "npm")
            list(APPEND BUILD_CMD "--")
        endif ()
        list(APPEND BUILD_CMD --outDir "${BUILD_DIST_DIR}" --emptyOutDir)

        set(VITE_STAMP
                "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-${ARG_NAMESPACE}-vite.stamp")
        add_custom_command(
                OUTPUT "${VITE_STAMP}"
                COMMAND ${CMAKE_COMMAND} -E env
                        --modify "PATH=path_list_prepend:${PM_DIR}"
                        --modify "PATH=path_list_prepend:${NODE_DIR}" -- ${BUILD_CMD}
                COMMAND ${CMAKE_COMMAND} -E touch "${VITE_STAMP}"
                WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
                DEPENDS ${VITE_SOURCES} ${ARG_DEPENDS}
                COMMENT "Building Vite project for ${TARGET} (${ARG_PACKAGE_MANAGER})"
                VERBATIM)

        # BASE_DIRECTORY preserves the relative path of each file under the
        # vite dist (e.g. "assets/foo.js" rather than just "foo.js"), so the
        # WebView scheme handler can resolve URLs like `app://local/assets/foo.js`.
        # Without this, ResEmbed flattens to basenames and nested assets 404.
        #
        # SCAN_DIR + DEPENDS=VITE_STAMP is the build-time-only chain: when
        # vite re-runs it touches the stamp, which triggers
        # ResourceGenerator to re-scan BUILD_DIST_DIR. Combined with the
        # depfile ResourceGenerator writes, content changes to any single
        # asset also re-embed without a CMake reconfigure.
        res_embed_add(${TARGET}
                SCAN_DIR       "${BUILD_DIST_DIR}"
                BASE_DIRECTORY "${BUILD_DIST_DIR}"
                NAMESPACE      ${ARG_NAMESPACE}
                CATEGORY       ${ARG_CATEGORY}
                DEPENDS        "${VITE_STAMP}")
        return()
    endif ()

    if (NOT EXISTS "${ARG_DIST_DIR}")
        message(FATAL_ERROR
                "eacp_webview_add_vite(${TARGET}): prebuilt Vite dist not found at "
                "${ARG_DIST_DIR}. Either run '${ARG_PACKAGE_MANAGER} run build' in "
                "${ARG_SOURCE_DIR} and commit the output, or configure with "
                "-DEACP_WEBVIEW_VITE_BUILD=ON to have CMake drive the Vite build.")
    endif ()

    res_embed_add(${TARGET}
            DIRECTORY      "${ARG_DIST_DIR}"
            BASE_DIRECTORY "${ARG_DIST_DIR}"
            NAMESPACE      ${ARG_NAMESPACE}
            CATEGORY       ${ARG_CATEGORY})
endfunction()
