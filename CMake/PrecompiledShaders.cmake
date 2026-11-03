set(EACP_PRECOMPILED_SHADERS_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(EACP_PRECOMPILE_SHADERS
        "Compile the shaders of apps wired with eacp_add_precompiled_shaders at build time and embed the blobs. When OFF, those apps compile every shader at run time."
        ON)

set(EACP_PRECOMPILE_SHADER_BACKENDS "" CACHE STRING
        "The backends every eacp_add_precompiled_shaders call compiles for, from metal, dxbc and spirv, overriding each call's BACKENDS. Empty: the call's BACKENDS, else the target platform's own (metal on Apple, dxbc on Windows, spirv on Linux and Android).")

# Whether `xcrun -sdk macosx metal` can run. An Xcode without its Metal Toolchain
# component still answers `xcrun --find metal`, so the compiler is asked for its
# version instead. Probed once per build directory; delete the cache entry after
# installing the toolchain.
function(eacp_probe_metal_toolchain out_var)
    if (NOT DEFINED EACP_METAL_TOOLCHAIN_FOUND)
        execute_process(
                COMMAND xcrun -sdk macosx metal --version
                RESULT_VARIABLE result
                OUTPUT_QUIET
                ERROR_QUIET)

        if (result EQUAL 0)
            set(found ON)
        else ()
            set(found OFF)
        endif ()

        set(EACP_METAL_TOOLCHAIN_FOUND ${found} CACHE INTERNAL
                "Whether xcrun -sdk macosx metal runs on this host")
    endif ()

    set(${out_var} ${EACP_METAL_TOOLCHAIN_FOUND} PARENT_SCOPE)
endfunction()

# The target platform's own backend: the one dialect its ShaderLibrary reads.
function(eacp_default_shader_backend out_var)
    if (APPLE)
        set(${out_var} metal PARENT_SCOPE)
    elseif (WIN32)
        set(${out_var} dxbc PARENT_SCOPE)
    else ()
        set(${out_var} spirv PARENT_SCOPE)
    endif ()
endfunction()

# Why this host cannot compile `backend`, or empty when it can.
function(eacp_shader_backend_unavailable_reason backend out_var)
    set(reason "")

    if (backend STREQUAL "metal")
        if (NOT APPLE)
            set(reason "Metal is compiled on a Mac only")
        else ()
            eacp_probe_metal_toolchain(metal_found)

            if (NOT metal_found)
                string(CONCAT reason
                        "the Metal Toolchain is not installed; install it with "
                        "xcodebuild -downloadComponent MetalToolchain, then "
                        "reconfigure with -UEACP_METAL_TOOLCHAIN_FOUND")
            endif ()
        endif ()
    elseif (backend STREQUAL "dxbc")
        if (NOT WIN32)
            set(reason "DXBC is compiled on a Windows host only")
        endif ()
    elseif (backend STREQUAL "spirv")
        if (NOT TARGET eacp-spirv)
            set(reason "eacp-spirv is not built (EACP_BUILD_SPIRV is OFF)")
        endif ()
    endif ()

    set(${out_var} "${reason}" PARENT_SCOPE)
endfunction()

# The backends an app's shaders are compiled for: EACP_PRECOMPILE_SHADER_BACKENDS
# when set, else the call's BACKENDS, else the platform's own; each one this
# host cannot compile is dropped with a STATUS line saying why.
function(eacp_resolve_shader_backends app requested out_var)
    if (EACP_PRECOMPILE_SHADER_BACKENDS)
        set(requested ${EACP_PRECOMPILE_SHADER_BACKENDS})
    endif ()

    if (NOT requested)
        eacp_default_shader_backend(requested)
    endif ()

    set(known metal dxbc spirv)
    set(backends "")

    foreach (backend IN LISTS requested)
        string(TOLOWER "${backend}" backend)

        if (NOT backend IN_LIST known)
            message(FATAL_ERROR "eacp_add_precompiled_shaders(${app}): unknown "
                    "backend '${backend}'; expected metal, dxbc or spirv")
        endif ()

        eacp_shader_backend_unavailable_reason(${backend} reason)

        if (reason)
            message(STATUS "${app}: not precompiling ${backend} shaders: ${reason}")
        else ()
            list(APPEND backends ${backend})
        endif ()
    endforeach ()

    list(REMOVE_DUPLICATES backends)
    set(${out_var} "${backends}" PARENT_SCOPE)
endfunction()

# Compiles an app's shaders at build time and embeds the blobs, which every
# ShaderLibrary then loads instead of compiling its source (GPU/README.md,
# "Compiling shaders at build time").
#
#   eacp_add_precompiled_shaders(<app>
#       SOURCES <shader TUs>...      built into the host tool; no main(). Their
#                                    EACP_SHADER_SET()s register themselves
#       [LINK <targets>...]          what those TUs link beyond eacp-gpu
#       [BACKENDS <metal|dxbc|spirv>...]  default: the platform's own
#       [METAL_SDKS <sdk>...]        default macosx
#       [METAL_MIN_OS <ver|sdk=ver>...])
#
# Builds <app>-shaders, a host executable of the sources and the generated
# main, runs it into <binary dir>/<app>-PrecompiledShaders and embeds every blob
# it lists under the EacpShaders category. Editing a shader TU rebuilds the
# tool, which reruns it. EACP_PRECOMPILE_SHADER_BACKENDS overrides BACKENDS for
# every call. Off with EACP_PRECOMPILE_SHADERS, when cross-compiling (iOS,
# Android), and when no requested backend can be compiled on this host: the app
# then compiles its shaders at run time.
function(eacp_add_precompiled_shaders app)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
            "" "SOURCES;SHADER_SETS;LINK;BACKENDS;METAL_SDKS;METAL_MIN_OS")

    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "eacp_add_precompiled_shaders(${app}): SOURCES is required")
    endif ()

    if (ARG_SHADER_SETS OR "SHADER_SETS" IN_LIST ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR
                "eacp_add_precompiled_shaders(${app}): SHADER_SETS is gone. Drop "
                "it: each EACP_SHADER_SET() in SOURCES now registers itself, so "
                "write `EACP_SHADER_SET() { visit<MyShader>(); }` with no name "
                "(eacp/GPU/Codegen/ShaderSets.h).")
    endif ()

    if (NOT EACP_PRECOMPILE_SHADERS)
        message(STATUS "${app}: precompiled shaders OFF (EACP_PRECOMPILE_SHADERS)")
        return()
    endif ()

    if (CMAKE_CROSSCOMPILING)
        message(STATUS "${app}: precompiled shaders skipped when cross-compiling; "
                "shaders compile at runtime")
        return()
    endif ()

    if (NOT TARGET eacp-shader-compiler OR NOT TARGET eacp-gpu)
        message(STATUS "${app}: no eacp-gpu; shaders compile at runtime")
        return()
    endif ()

    eacp_resolve_shader_backends(${app} "${ARG_BACKENDS}" backends)

    if (NOT backends)
        message(STATUS "${app}: no shader backend to precompile on this host; "
                "shaders compile at runtime")
        return()
    endif ()

    set(flags "")

    if ("metal" IN_LIST backends)
        if (NOT ARG_METAL_SDKS)
            set(ARG_METAL_SDKS macosx)
        endif ()

        if (NOT ARG_METAL_MIN_OS AND CMAKE_OSX_DEPLOYMENT_TARGET)
            set(ARG_METAL_MIN_OS "macosx=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif ()

        foreach (sdk IN LISTS ARG_METAL_SDKS)
            list(APPEND flags --metal-sdk ${sdk})
        endforeach ()

        foreach (version IN LISTS ARG_METAL_MIN_OS)
            list(APPEND flags --metal-min-os ${version})
        endforeach ()
    else ()
        list(APPEND flags --no-metal)
    endif ()

    if (NOT "dxbc" IN_LIST backends)
        list(APPEND flags --no-dxbc)
    endif ()

    if (NOT "spirv" IN_LIST backends)
        list(APPEND flags --no-spirv)
    endif ()

    list(JOIN backends ", " backend_names)
    message(STATUS "${app}: precompiled shaders (${backend_names})")

    # Neither directory may be named <app>-Shaders: beside the <app>-shaders
    # executable, a case-insensitive file system (macOS, Windows) sees one name.
    set(tool ${app}-shaders)
    set(tool_main "${CMAKE_CURRENT_BINARY_DIR}/${app}-ShaderHost/ShaderHostMain.cpp")
    configure_file("${EACP_PRECOMPILED_SHADERS_DIR}/ShaderHostMain.cpp.in"
            "${tool_main}" @ONLY)

    add_executable(${tool} "${tool_main}" ${ARG_SOURCES})
    target_link_libraries(${tool} PRIVATE eacp-shader-compiler eacp-gpu ${ARG_LINK})
    set_default_warnings_level(${tool})
    set_target_properties(${tool} PROPERTIES EXCLUDE_FROM_ALL ON)

    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${app}-PrecompiledShaders")
    set(manifest "${output_dir}/manifest.txt")

    add_custom_command(
            OUTPUT "${manifest}"
            COMMAND $<TARGET_FILE:${tool}>
                    --out "${output_dir}"
                    --manifest "${manifest}"
                    ${flags}
            DEPENDS ${tool}
            COMMENT "Compiling shaders for ${app} (${backend_names})"
            VERBATIM)

    # MANIFEST mode depends on the manifest itself and, through its depfile,
    # on every blob the manifest lists.
    res_embed_add(${app}
            MANIFEST "${manifest}"
            BASE_DIRECTORY "${output_dir}"
            NAMESPACE EacpShaders
            CATEGORY EacpShaders)
endfunction()
