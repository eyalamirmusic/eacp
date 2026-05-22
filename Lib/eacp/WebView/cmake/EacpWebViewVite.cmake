option(EACP_WEBVIEW_VITE_BUILD "Let CMake drive '<pm> install' + 'vite build' for embedded webview apps. When OFF, the prebuilt dist committed at SOURCE_DIR/dist is embedded as-is. NOTE: when ON, the first 'cmake --build' produces the dist but embeds an empty resource registry (configure-time glob saw no files); CONFIGURE_DEPENDS makes the next build invocation reglob and embed for real." ON)

set(EACP_WEBVIEW_PACKAGE_MANAGER "npm" CACHE STRING
        "Package manager used to drive embedded vite builds (e.g. npm, pnpm, yarn, bun). Can be overridden per-call via the PACKAGE_MANAGER argument to eacp_webview_add_vite.")

function(eacp_webview_add_vite TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
            "SOURCE_DIR;DIST_DIR;NAMESPACE;CATEGORY;PACKAGE_MANAGER" "")

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

    if (EACP_WEBVIEW_VITE_BUILD AND EXISTS "${ARG_SOURCE_DIR}/package.json")
        find_program(EACP_WEBVIEW_PM_EXECUTABLE_${ARG_PACKAGE_MANAGER}
                NAMES ${ARG_PACKAGE_MANAGER}.cmd ${ARG_PACKAGE_MANAGER}
                REQUIRED)
        set(PM_EXECUTABLE "${EACP_WEBVIEW_PM_EXECUTABLE_${ARG_PACKAGE_MANAGER}}")
        set(BUILD_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-vite-dist")

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

        # ResourceGenerator rejects an empty input list, so seed BUILD_DIST_DIR
        # with a placeholder. Vite's --emptyOutDir wipes the dist dir at build
        # time, so the custom_command copies the template back afterwards.
        # Copying (rather than touching) keeps the placeholder non-empty, so
        # the embedded C array isn't a zero-length extension.
        file(MAKE_DIRECTORY "${BUILD_DIST_DIR}")
        set(VITE_PLACEHOLDER_TEMPLATE
                "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-vite-placeholder.txt")
        set(VITE_PLACEHOLDER "${BUILD_DIST_DIR}/placeholder.txt")
        file(WRITE "${VITE_PLACEHOLDER_TEMPLATE}"
                "Vite build placeholder — kept so the embedded resource list is non-empty.\n")
        configure_file("${VITE_PLACEHOLDER_TEMPLATE}" "${VITE_PLACEHOLDER}" COPYONLY)

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

        set(VITE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-vite.stamp")
        add_custom_command(
                OUTPUT "${VITE_STAMP}"
                COMMAND ${BUILD_CMD}
                COMMAND ${CMAKE_COMMAND} -E copy
                        "${VITE_PLACEHOLDER_TEMPLATE}" "${VITE_PLACEHOLDER}"
                COMMAND ${CMAKE_COMMAND} -E touch "${VITE_STAMP}"
                WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
                DEPENDS ${VITE_SOURCES}
                COMMENT "Building Vite project for ${TARGET} (${ARG_PACKAGE_MANAGER})"
                VERBATIM)

        target_sources(${TARGET} PRIVATE "${VITE_STAMP}")
        set_source_files_properties("${VITE_STAMP}" PROPERTIES
                GENERATED TRUE HEADER_FILE_ONLY TRUE)

        # BASE_DIRECTORY preserves the relative path of each file under the
        # vite dist (e.g. "assets/foo.js" rather than just "foo.js"), so the
        # WebView scheme handler can resolve URLs like `app://local/assets/foo.js`.
        # Without this, ResEmbed flattens to basenames and nested assets 404.
        res_embed_add(${TARGET}
                DIRECTORY      "${BUILD_DIST_DIR}"
                BASE_DIRECTORY "${BUILD_DIST_DIR}"
                NAMESPACE      ${ARG_NAMESPACE}
                CATEGORY       ${ARG_CATEGORY})
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
