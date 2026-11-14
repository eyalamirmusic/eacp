# A runtime-loaded eacp plugin: a MODULE library that statically links its
# own eacp copy. eacp and its dependencies compile with hidden visibility, so
# the module re-exports none of their symbols; plugin entry points are marked
# with EACP_PLUGIN_EXPORT (Core/Plugins/PluginExport.h), and the visibility
# of the plugin's own remaining code is the plugin author's choice. A build
# with eacp_add_hot_reload ON compiles eacp at default visibility instead, so
# plugins built in it re-export eacp and lose that isolation.
#
#   eacp_add_plugin(MyPlugin Plugin.cpp)
#   target_link_libraries(MyPlugin PRIVATE eacp-core)
function(eacp_add_plugin target)
    add_library(${target} MODULE ${ARGN})

    set_target_properties(${target} PROPERTIES
            PREFIX ""
            # CMake defines <target>_EXPORTS for a MODULE by default. eacp marks
            # its plugin entry points with EACP_PLUGIN_EXPORT and nothing reads
            # the generated macro, so drop it rather than leave every plugin
            # carrying a define the shared PCH image cannot match.
            DEFINE_SYMBOL ""
            POSITION_INDEPENDENT_CODE ON)

    # Keep the plugin's own sources free of STB_GNU_UNIQUE symbols so glibc can
    # unmap it on the last close -- see the eacp-wide flag in the root
    # CMakeLists that covers the eacp libraries linked in here.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fno-gnu-unique)
    endif ()

    set_default_target_setting(${target})
    eacp_enable_unity_build(${target})
endfunction()

# For an executable whose source says EACP_CLAIM_COPY (Core/Plugins/ClaimCopy.h).
# A Linux executable exports nothing unless asked, so the claim is named to the
# linker (GNU ld 2.35+, gold, lld 11+, mold). The define satisfies the macro's
# static_assert, so a forgotten call fails the build on every platform.
function(eacp_claim_copy executable)
    target_compile_definitions(${executable} PRIVATE EACP_CLAIM_COPY_LINKED=1)

    if (LINUX)
        target_link_options(${executable} PRIVATE
                "LINKER:--export-dynamic-symbol=eacpClaimedCopy")
    endif ()
endfunction()

function(eacp_collect_link_targets target out_var)
    set(found ${${out_var}})
    get_target_property(links ${target} LINK_LIBRARIES)
    get_target_property(interface_links ${target} INTERFACE_LINK_LIBRARIES)

    foreach (link IN LISTS links interface_links)
        string(REGEX REPLACE "\\$<LINK_ONLY:([^>]*)>" "\\1" link "${link}")

        if (WIN32)
            eacp_unwrapped_link_item(${target} "${link}" link)
        endif ()

        if (NOT TARGET "${link}")
            continue()
        endif ()

        get_target_property(aliased ${link} ALIASED_TARGET)

        if (aliased)
            set(link ${aliased})
        endif ()

        get_target_property(imported ${link} IMPORTED)

        if (imported OR link IN_LIST found)
            continue()
        endif ()

        list(APPEND found ${link})
        eacp_collect_link_targets(${link} found)
    endforeach ()

    set(${out_var} ${found} PARENT_SCOPE)
endfunction()

# A link item with the wrappers that change nothing at build time taken off:
# $<LINK_ONLY:x>, $<BUILD_INTERFACE:x> and $<BUILD_LOCAL_INTERFACE:x>, and empty
# for $<INSTALL_INTERFACE:x>. Any other generator expression that names a target
# is refused: whether it links that target is only known at generate time, and
# the export list and the whole-archive link need to know now.
function(eacp_unwrapped_link_item owner item out_var)
    set(previous "")

    while (NOT item STREQUAL previous)
        set(previous "${item}")
        string(REGEX REPLACE "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):(.*)>$"
                "\\2" item "${item}")
    endwhile ()

    if (item MATCHES "^\\$<INSTALL_INTERFACE:")
        set(item "")
    elseif (item MATCHES "^\\$<.*:([^:<>]+)>$" AND TARGET "${CMAKE_MATCH_1}")
        message(FATAL_ERROR "${owner} links '${item}', a generator expression "
                "around the target ${CMAKE_MATCH_1}. Windows hot reload must know "
                "at configure time whether that target is in the app's closure: "
                "link it unconditionally, or pass HOT_RELOAD OFF.")
    endif ()

    set(${out_var} "${item}" PARENT_SCOPE)
endfunction()

# Windows: what the module must still link itself, since the app's export list
# covers only symbols defined in objects it reads. Every link item of the
# module's links and their closure that is not a target (winmm, d3d12, a path,
# a flag), and every imported or shared library target, in first-seen order.
function(eacp_collect_system_links owner links out_var)
    set(found "")
    set(visited "")
    set(pending ${links})

    while (pending)
        list(POP_FRONT pending link)
        eacp_unwrapped_link_item(${owner} "${link}" link)

        if (link MATCHES "^(|debug|optimized|general)$" OR link IN_LIST visited)
            continue()
        endif ()

        list(APPEND visited "${link}")

        if (NOT TARGET "${link}")
            if (NOT link MATCHES "^\\$<COMPILE_ONLY:")
                list(APPEND found "${link}")
            endif ()

            continue()
        endif ()

        get_target_property(aliased ${link} ALIASED_TARGET)

        if (aliased)
            set(link ${aliased})
        endif ()

        get_target_property(imported ${link} IMPORTED)
        get_target_property(type ${link} TYPE)

        if (imported OR type MATCHES "^(SHARED|MODULE)_LIBRARY$")
            if (NOT type STREQUAL "INTERFACE_LIBRARY")
                list(APPEND found ${link})
                continue()
            endif ()
        endif ()

        if (NOT imported)
            get_target_property(direct ${link} LINK_LIBRARIES)

            if (direct AND type MATCHES "^(STATIC|OBJECT)_LIBRARY$")
                list(APPEND pending ${direct})
            endif ()
        endif ()

        get_target_property(interface ${link} INTERFACE_LINK_LIBRARIES)

        if (interface)
            list(APPEND pending ${interface})
        endif ()
    endwhile ()

    list(REMOVE_DUPLICATES found)
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

function(eacp_set_default_visibility target)
    set_target_properties(${target} PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
            OBJC_VISIBILITY_PRESET default
            OBJCXX_VISIBILITY_PRESET default
            VISIBILITY_INLINES_HIDDEN OFF)
endfunction()

function(eacp_link_hot_reload app module)
    get_target_property(module_links ${module} LINK_LIBRARIES)

    if (NOT module_links)
        set(module_links "")
    endif ()

    set(compile_only "")

    foreach (link IN LISTS module_links)
        if (NOT TARGET "${link}")
            continue()
        endif ()

        list(APPEND compile_only "$<COMPILE_ONLY:${link}>")
        get_target_property(imported ${link} IMPORTED)

        if (NOT imported)
            add_dependencies(${module} ${link})
        endif ()
    endforeach ()

    set(system_links "")

    if (WIN32)
        eacp_collect_system_links(${app} "${module_links}" system_links)
    endif ()

    set_target_properties(${module} PROPERTIES LINK_LIBRARIES "${compile_only}")
    target_link_libraries(${app} PRIVATE ${module_links})

    get_target_property(module_sources ${module} SOURCES)
    get_target_property(module_dir ${module} SOURCE_DIR)
    set(watched "")

    foreach (source IN LISTS module_sources)
        if (source MATCHES "\\$<" OR source MATCHES "NOTFOUND$")
            continue()
        endif ()

        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${module_dir} NORMALIZE)
        list(APPEND watched ${source})
    endforeach ()

    list(JOIN watched "|" watched)
    target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD_SOURCES="${watched}")

    get_target_property(direct ${app} LINK_LIBRARIES)
    set(closure "")
    eacp_collect_link_targets(${app} closure)
    set(archives "")

    foreach (target IN LISTS closure)
        get_target_property(type ${target} TYPE)

        if (type STREQUAL "STATIC_LIBRARY")
            list(APPEND archives ${target})
        endif ()

        if (type MATCHES "^(STATIC|OBJECT)_LIBRARY$")
            eacp_set_default_visibility(${target})

            # The export list is read from these objects, and /GL and LLVM
            # bitcode carry no COFF symbol table. Like the visibility above,
            # this is the whole build directory's setting.
            if (WIN32)
                eacp_disable_link_time_optimization(${target})
            endif ()
        endif ()
    endforeach ()

    set(links "")

    foreach (link IN LISTS direct)
        if (NOT link IN_LIST archives)
            list(APPEND links "${link}")
        endif ()
    endforeach ()

    foreach (archive IN LISTS archives)
        set(whole "$<LINK_LIBRARY:WHOLE_ARCHIVE,${archive}>")

        if (archive IN_LIST direct)
            list(APPEND links "${whole}")
        else ()
            list(APPEND links "$<LINK_ONLY:${whole}>")
        endif ()

        set_target_properties(${app} PROPERTIES
                LINK_LIBRARY_OVERRIDE_${archive} WHOLE_ARCHIVE)
    endforeach ()

    set_target_properties(${app} PROPERTIES
            LINK_LIBRARIES "${links}"
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

    if (WIN32)
        eacp_export_hot_reload_closure(${app} ${module} "${archives}" "${closure}"
                "${system_links}")
    endif ()
endfunction()

# The configurations this build directory builds in which eacp's libraries are
# link-time optimised: Release, where set_default_target_setting turns it on,
# and any configuration CMAKE_INTERPROCEDURAL_OPTIMIZATION[_<CONFIG>] turns it on
# for. A multi-config generator answers for every configuration it has.
function(eacp_link_time_optimized_configurations out_var)
    set(configurations ${CMAKE_BUILD_TYPE})

    if (CMAKE_CONFIGURATION_TYPES)
        set(configurations ${CMAKE_CONFIGURATION_TYPES})
    endif ()

    set(optimized "")

    foreach (config IN LISTS configurations)
        string(TOUPPER "${config}" upper)

        if (upper STREQUAL "RELEASE" OR CMAKE_INTERPROCEDURAL_OPTIMIZATION
                OR CMAKE_INTERPROCEDURAL_OPTIMIZATION_${upper})
            list(APPEND optimized ${config})
        endif ()
    endforeach ()

    set(${out_var} ${optimized} PARENT_SCOPE)
endfunction()

function(eacp_disable_link_time_optimization target)
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

    foreach (config Debug Release RelWithDebInfo MinSizeRel
            ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
        string(TOUPPER "${config}" config)
        set_target_properties(${target} PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION_${config} OFF)
    endforeach ()
endfunction()

# Why a Windows app cannot be in dev mode in this build directory, or empty.
function(eacp_windows_hot_reload_refusal app out_var)
    set(reason "")
    eacp_static_msvc_runtime_reason(static_runtime ${app})

    if (NOT MSVC)
        set(reason "Windows dev mode needs cl or clang-cl")
    elseif (NOT CMAKE_GENERATOR MATCHES "^Ninja")
        # The app's objects feed the export list its own link needs, so its
        # compiles, the list and its link must interleave across targets,
        # which only Ninja's single build graph does.
        set(reason "Windows dev mode needs a Ninja generator, not "
                "${CMAKE_GENERATOR}")
    elseif (NOT TARGET eacp-export-list-tool)
        set(reason "no eacp-export-list-tool to list the app's exports "
                "(it is not built when cross-compiling)")
    elseif (NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|ARM64)$")
        set(reason "Windows dev mode supports x64 and ARM64, not "
                "'${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'")
    elseif (static_runtime)
        set(reason "${static_runtime}")
    endif ()

    string(JOIN "" reason ${reason})
    set(${out_var} "${reason}" PARENT_SCOPE)
endfunction()

# One C runtime, one heap: a module allocating what the app frees, or the other
# way round, needs both on the DLL runtime (/MD). Why the given targets or the
# build directory's flags are on the static one, or empty.
function(eacp_static_msvc_runtime_reason out_var)
    set(reason "")

    foreach (target IN LISTS ARGN)
        get_target_property(runtime ${target} MSVC_RUNTIME_LIBRARY)

        if (runtime AND NOT runtime MATCHES "DLL")
            set(reason "Windows dev mode needs the DLL C runtime (/MD), and "
                    "${target}'s MSVC_RUNTIME_LIBRARY is ${runtime}")
            break()
        endif ()
    endforeach ()

    foreach (config "" Debug Release RelWithDebInfo MinSizeRel
            ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
        if (config)
            string(TOUPPER "_${config}" config)
        endif ()

        foreach (language C CXX)
            if (NOT reason
                    AND CMAKE_${language}_FLAGS${config} MATCHES "(^| )[-/]MTd?( |$)")
                set(reason "Windows dev mode needs the DLL C runtime (/MD), and "
                        "CMAKE_${language}_FLAGS${config} asks for the static one")
            endif ()
        endforeach ()
    endforeach ()

    string(JOIN "" reason ${reason})
    set(${out_var} "${reason}" PARENT_SCOPE)
endfunction()

# Windows has no load-time binding to an executable's symbols, so the app names
# them: an export list over its own objects and every archive in its closure
# (eacp_add_export_list, Lib/eacp/ExportList/README.md) is the app's /DEF, and
# an import library made from that list alone is the module's only link input.
# The module never depends on the app target, so building it never touches the
# running, locked exe. The app's link waits for the list and, through a stamp
# made once after the module's first build, for the module, so a fresh build
# directory has both and a module that changes relinks nothing but itself.
function(eacp_export_hot_reload_closure app module archives closure system_links)
    # The app and the build directory's flags were checked before the module
    # was made; a closure library that picks the static runtime itself is only
    # known now, too late to fall back to static mode.
    eacp_static_msvc_runtime_reason(static_runtime ${module} ${archives})

    if (static_runtime)
        message(FATAL_ERROR "${app}: ${static_runtime}. Use "
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL there, or pass "
                "HOT_RELOAD OFF to eacp_add_hot_reload for ${app}.")
    endif ()

    get_target_property(app_dir ${app} BINARY_DIR)
    set(stem "${app_dir}/${app}-hot-reload")

    if (CMAKE_CONFIGURATION_TYPES)
        set(stem "${app_dir}/$<CONFIG>/${app}-hot-reload")
    endif ()

    get_target_property(app_options ${app} LINK_OPTIONS)
    get_target_property(app_flags ${app} LINK_FLAGS)
    get_target_property(app_sources ${app} SOURCES)

    if ("${app_options};${app_flags}" MATCHES "[-/][Dd][Ee][Ff]:"
            OR app_sources MATCHES "\\.def(;|$)")
        message(FATAL_ERROR "${app} links with a module-definition file of its "
                "own, and Windows hot reload links it with the export list "
                "instead: a PE image takes one. Pass HOT_RELOAD OFF for ${app}.")
    endif ()

    set(def "${stem}.def")
    set(import_library "${stem}-exports.lib")
    set(module_built "${stem}-module-built.stamp")

    get_target_property(direct ${app} LINK_LIBRARIES)
    set(object_libraries "")

    foreach (target IN LISTS closure)
        get_target_property(type ${target} TYPE)

        if (type STREQUAL "OBJECT_LIBRARY" AND target IN_LIST direct)
            list(APPEND object_libraries ${target})
        endif ()
    endforeach ()

    eacp_add_export_list(${app}-hot-reload-exports
            DEF "${def}"
            IMAGE "$<TARGET_FILE_NAME:${app}>"
            TARGETS ${app} ${object_libraries} ${archives}
            IMPORT_LIBRARY "${import_library}")

    get_target_property(module_links ${module} LINK_LIBRARIES)

    if (NOT module_links)
        set(module_links "")
    endif ()

    list(APPEND module_links "${import_library}" ${system_links})
    set_target_properties(${module} PROPERTIES LINK_LIBRARIES "${module_links}")
    add_dependencies(${module} ${app}-hot-reload-exports)

    # The image records only its PDB's file name, so a debugger takes the copy
    # ReloadableLibrary stages beside each generation and never holds the one
    # the next link writes (LNK1201).
    target_link_options(${module} PRIVATE "/PDBALTPATH:%_PDB%")

    add_custom_command(
            OUTPUT "${module_built}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${module_built}"
            VERBATIM)
    add_custom_target(${app}-hot-reload-module DEPENDS "${module_built}")
    add_dependencies(${app}-hot-reload-module ${module})

    target_link_options(${app} PRIVATE "/DEF:${def}")
    set_property(TARGET ${app} APPEND PROPERTY
            LINK_DEPENDS "${def}" "${module_built}")

    eacp_disable_link_time_optimization(${app})
    eacp_disable_link_time_optimization(${module})
    eacp_link_time_optimized_configurations(optimized)

    if (optimized)
        list(JOIN optimized ", " optimized)
        message(STATUS "${app}: link-time optimisation off in ${optimized} for the "
                "app, its module and every library in its closure, which the "
                "export list reads (EACP_HOT_RELOAD=ON)")
    endif ()
endfunction()

# Configure ran in the compiler's environment (vcvars' INCLUDE, LIB and PATH),
# and Ninja with cl compiles in whatever environment it is started from. An app
# started from Explorer or a plain shell has none, so the configure's is written
# down here and the rebuild replays it.
function(eacp_write_hot_reload_build_environment out_var)
    set(file "${CMAKE_BINARY_DIR}/eacp-hot-reload-build.env")
    set(text "")

    foreach (name INCLUDE LIB LIBPATH EXTERNAL_INCLUDE PATH)
        if (DEFINED ENV{${name}})
            string(APPEND text "${name}=$ENV{${name}}\n")
        endif ()
    endforeach ()

    set(previous "")

    if (EXISTS "${file}")
        file(READ "${file}" previous)
    endif ()

    if (NOT previous STREQUAL text)
        file(WRITE "${file}" "${text}")
    endif ()

    set(${out_var} "${file}" PARENT_SCOPE)
endfunction()

# Deferred to the end of the configure, once the app's and the library's links
# are final. In dev mode the library is a module that links nothing of eacp: its
# links go to the app, which exports them, and reach the module compile-only
# (on Windows the module links the import library of the app's export list).
# Windows dev mode needs Ninja and cl or clang-cl, and is static elsewhere with
# a status line saying why.
#
#   eacp_add_hot_reload(MyApp MyAppLayout SOURCES Layout.cpp [HOT_RELOAD <bool>])
#   target_link_libraries(MyAppLayout PRIVATE eacp-ui)
function(eacp_add_hot_reload app library)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "HOT_RELOAD" "SOURCES")

    get_property(already TARGET ${app} PROPERTY EACP_HOT_RELOAD_LIBRARY SET)

    if (already)
        get_target_property(first ${app} EACP_HOT_RELOAD_LIBRARY)
        message(FATAL_ERROR "eacp_add_hot_reload: ${app} already has the library "
                "${first}; an app has one hot-reload library (EACP_RELOADABLE "
                "defines one table per app), so list every function in it")
    endif ()

    set_target_properties(${app} PROPERTIES EACP_HOT_RELOAD_LIBRARY ${library})

    set(enabled OFF)

    if (EACP_HOT_RELOAD)
        set(enabled ON)
    endif ()

    if (DEFINED ARG_HOT_RELOAD AND NOT ARG_HOT_RELOAD STREQUAL "")
        if (ARG_HOT_RELOAD)
            set(enabled ON)
        else ()
            set(enabled OFF)
        endif ()
    endif ()

    if (IOS OR ANDROID)
        set(enabled OFF)
    endif ()

    set(refusal "")

    if (enabled AND WIN32)
        eacp_windows_hot_reload_refusal(${app} refusal)

        if (refusal)
            set(enabled OFF)
            set(refusal " (${refusal})")
        endif ()
    endif ()

    # Windows dev mode turns link-time optimisation off for the whole closure,
    # so a configuration that has it stays as optimised as it was unless the
    # build directory itself asks for dev mode; an app's own HOT_RELOAD is not
    # enough there.
    if (enabled AND WIN32 AND NOT EACP_HOT_RELOAD)
        eacp_link_time_optimized_configurations(optimized)

        if (optimized)
            list(JOIN optimized ", " optimized)
            set(refusal "Windows dev mode needs link-time optimisation off, "
                    "which ${optimized} has on. Configure with "
                    "-DEACP_HOT_RELOAD=ON to turn it off for the closure")
            string(JOIN "" refusal ${refusal})
            set(enabled OFF)
            set(refusal " (${refusal})")
        endif ()
    endif ()

    set_target_properties(${app} PROPERTIES EACP_HOT_RELOAD_ENABLED ${enabled})

    if (NOT enabled)
        message(STATUS "${app}: hot reload OFF${refusal}")
        add_library(${library} STATIC ${ARG_SOURCES})
        set_target_properties(${library} PROPERTIES UNITY_BUILD OFF)
        set_default_warnings_level(${library})
        target_link_libraries(${app} PRIVATE ${library})
        target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD=0)
        return()
    endif ()

    message(STATUS "${app}: hot reload ON")

    eacp_set_default_visibility(${app})

    eacp_add_plugin(${library} ${ARG_SOURCES})
    target_compile_definitions(${library} PRIVATE EACP_HOT_RELOAD_MODULE=1)

    # On Windows the module links against the app's export list, which is made
    # from the app's objects, so the app cannot depend on it as a target; its
    # link waits for the module instead (eacp_export_hot_reload_closure).
    if (WIN32)
        eacp_write_hot_reload_build_environment(build_environment)
        target_compile_definitions(${app} PRIVATE
                EACP_HOT_RELOAD_BUILD_ENV="${build_environment}"
                EACP_HOT_RELOAD_SYMBOLS="$<TARGET_PDB_FILE:${library}>")
    else ()
        set_target_properties(${app} PROPERTIES ENABLE_EXPORTS ON)
        add_dependencies(${app} ${library})
    endif ()

    eacp_set_default_visibility(${library})
    set_target_properties(${library} PROPERTIES
            UNITY_BUILD OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

    if (APPLE)
        target_link_options(${app} PRIVATE LINKER:-export_dynamic)
        target_link_options(${library} PRIVATE LINKER:-undefined,dynamic_lookup)
    endif ()

    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
                CALL eacp_link_hot_reload [[${app}]] [[${library}]])")

    target_compile_definitions(${app} PRIVATE
            EACP_HOT_RELOAD=1
            EACP_HOT_RELOAD_LIBRARY="$<TARGET_FILE:${library}>"
            EACP_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
            EACP_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
            EACP_HOT_RELOAD_CONFIG="$<CONFIG>"
            EACP_HOT_RELOAD_TARGET="${library}"
            EACP_HOT_RELOAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
