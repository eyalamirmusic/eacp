include(CPM)

if (NOT ANDROID)
    CPMAddPackage(
            NAME ResEmbed
            GITHUB_REPOSITORY eyalamirmusic/ResEmbed
            GIT_TAG main)
else ()
    # ResEmbed's generator is a C++ program it builds for the build machine at
    # configure time, with whatever compiler that machine has on the PATH, and a
    # Windows host has none outside a Visual Studio prompt. So an Android build
    # takes ResEmbed's runtime and res_embed_add as they are, and runs the
    # generator as CMake script instead (ResEmbedGenerator.cmake), which needs
    # nothing but the CMake already running.
    CPMAddPackage(
            NAME ResEmbed
            GITHUB_REPOSITORY eyalamirmusic/ResEmbed
            GIT_TAG main
            DOWNLOAD_ONLY YES
            ${EACP_FETCH_QUIET})

    function(eacp_add_resembed_runtime)
        set(CMAKE_CXX_STANDARD 20)
        set(CMAKE_CXX_VISIBILITY_PRESET hidden)
        set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)
        add_subdirectory("${ResEmbed_SOURCE_DIR}/Lib" "${ResEmbed_BINARY_DIR}/Lib")
    endfunction()

    if (NOT TARGET ResEmbed)
        eacp_add_resembed_runtime()
        include("${ResEmbed_SOURCE_DIR}/CMake/ResEmbed.cmake")

        # res_embed_add runs `ResourceGenerator generate <args>`; this launcher
        # turns that into `cmake -P ResEmbedGenerator.cmake -- generate <args>`.
        set(script "${CMAKE_CURRENT_LIST_DIR}/ResEmbedGenerator.cmake")

        if (CMAKE_HOST_WIN32)
            set(launcher "${ResEmbed_BINARY_DIR}/ResourceGenerator.cmd")
            file(WRITE "${launcher}" "@\"${CMAKE_COMMAND}\" -P \"${script}\" -- %*\r\n")
        else ()
            set(launcher "${ResEmbed_BINARY_DIR}/ResourceGenerator")
            file(WRITE "${launcher}"
                    "#!/bin/sh\nexec \"${CMAKE_COMMAND}\" -P \"${script}\" -- \"$@\"\n")
            file(CHMOD "${launcher}" PERMISSIONS OWNER_READ OWNER_WRITE
                    OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif ()

        add_executable(ResourceGenerator IMPORTED GLOBAL)
        set_target_properties(ResourceGenerator PROPERTIES IMPORTED_LOCATION "${launcher}")
    endif ()
endif ()
