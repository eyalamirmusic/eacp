option(EACP_WEBVIEW_VITE_DEV "Build embedded webview apps against a live Vite dev server" OFF)
set(EACP_WEBVIEW_VITE_DEV_PORT "5173" CACHE STRING
        "Localhost port of the Vite dev server when EACP_WEBVIEW_VITE_DEV is ON")

function(eacp_webview_add_vite TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SOURCE_DIR;NAMESPACE;CATEGORY" "")

    if (NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "eacp_webview_add_vite: SOURCE_DIR is required")
    endif ()
    if (NOT ARG_NAMESPACE)
        set(ARG_NAMESPACE "Resources")
    endif ()
    if (NOT ARG_CATEGORY)
        set(ARG_CATEGORY "Resources")
    endif ()

    find_program(NPM_EXECUTABLE npm REQUIRED)

    set(DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-vite-dist")

    if (EACP_WEBVIEW_VITE_DEV)
        target_compile_definitions(${TARGET} PRIVATE
                EACP_WEBVIEW_DEV_SERVER_URL="http://localhost:${EACP_WEBVIEW_VITE_DEV_PORT}")
        file(MAKE_DIRECTORY "${DIST_DIR}")
        res_embed_add(${TARGET}
                DIRECTORY "${DIST_DIR}"
                NAMESPACE ${ARG_NAMESPACE}
                CATEGORY ${ARG_CATEGORY})
        return()
    endif ()

    if (NOT EXISTS "${ARG_SOURCE_DIR}/node_modules")
        message(STATUS "eacp_webview_add_vite(${TARGET}): running npm install")
        execute_process(
                COMMAND ${NPM_EXECUTABLE} install
                WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
                RESULT_VARIABLE NPM_INSTALL_RESULT)
        if (NOT NPM_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "npm install failed for ${TARGET}")
        endif ()
    endif ()

    if (NOT EXISTS "${DIST_DIR}")
        message(STATUS "eacp_webview_add_vite(${TARGET}): initial Vite build")
        execute_process(
                COMMAND ${NPM_EXECUTABLE} run build -- --outDir "${DIST_DIR}" --emptyOutDir
                WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
                RESULT_VARIABLE NPM_BUILD_RESULT)
        if (NOT NPM_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "vite build failed for ${TARGET}")
        endif ()
    endif ()

    file(GLOB_RECURSE VITE_SOURCES CONFIGURE_DEPENDS
            "${ARG_SOURCE_DIR}/src/*"
            "${ARG_SOURCE_DIR}/public/*"
            "${ARG_SOURCE_DIR}/index.html"
            "${ARG_SOURCE_DIR}/package.json"
            "${ARG_SOURCE_DIR}/vite.config.*"
            "${ARG_SOURCE_DIR}/tsconfig*.json")

    set(VITE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-vite.stamp")
    add_custom_command(
            OUTPUT "${VITE_STAMP}"
            COMMAND ${NPM_EXECUTABLE} run build -- --outDir "${DIST_DIR}" --emptyOutDir
            COMMAND ${CMAKE_COMMAND} -E touch "${VITE_STAMP}"
            WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
            DEPENDS ${VITE_SOURCES}
            COMMENT "Building Vite project for ${TARGET}"
            VERBATIM)

    add_custom_target(${TARGET}-vite-build DEPENDS "${VITE_STAMP}")
    add_dependencies(${TARGET} ${TARGET}-vite-build)

    res_embed_add(${TARGET}
            DIRECTORY "${DIST_DIR}"
            NAMESPACE ${ARG_NAMESPACE}
            CATEGORY ${ARG_CATEGORY})
endfunction()
