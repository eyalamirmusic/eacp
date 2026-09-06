include(CPM)

# glslang stamps FOLDER "glslang" on its own targets, which would sit them next
# to the eacp tree in an IDE instead of alongside the other CPM dependencies,
# which simply inherit CMAKE_FOLDER.
function(eacp_set_glslang_ide_folder dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)

    foreach (target IN LISTS targets)
        set_target_properties(${target} PROPERTIES FOLDER "${CMAKE_FOLDER}")
    endforeach ()

    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)

    foreach (subdir IN LISTS subdirs)
        eacp_set_glslang_ide_folder("${subdir}")
    endforeach ()
endfunction()

# Everything that would drag in a second dependency tree is off: SPIRV-Tools
# (ENABLE_OPT), googletest (GLSLANG_TESTS), the /External checkout
# (BUILD_EXTERNAL). What is left is one self-contained static archive.
# glslang 16.5.0 generates its build_info.h with parse_version.cmake and
# configure_file, so nothing here needs Python at configure or build time.
#
# The settings live in a function so none of them leaks past this fetch:
# add_subdirectory (which is how CPM adds a package) inherits the caller's
# variables, and glslang's cmake_minimum_required(3.22.1) puts CMP0077 at NEW,
# so a normal variable wins over its option() without touching the cache.
function(eacp_add_glslang)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_EXTERNAL OFF)
    set(ENABLE_OPT OFF)
    set(ENABLE_HLSL OFF)
    set(ENABLE_SPIRV ON)
    set(ENABLE_GLSLANG_BINARIES OFF)
    set(ENABLE_GLSLANG_JS OFF)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)

    # eacp compiles with exceptions and RTTI; glslang's defaults are the
    # opposite. On MSVC its -fno-exceptions equivalent is /D_HAS_EXCEPTIONS=0,
    # which changes the layout of the std::string and std::vector its public API
    # passes across the library boundary. Matching eacp's model keeps one ABI on
    # every compiler.
    set(ENABLE_EXCEPTIONS ON)
    set(ENABLE_RTTI ON)

    # A CI lane sets no CPM_SOURCE_CACHE, so the fetch is a full clone of a
    # fifteen-year history: 136 MB against 74 MB shallow, for the same 8 seconds.
    CPMAddPackage(
            NAME glslang
            GITHUB_REPOSITORY KhronosGroup/glslang
            GIT_TAG 16.5.0
            GIT_SHALLOW YES
            SYSTEM YES)

    eacp_set_glslang_ide_folder("${glslang_SOURCE_DIR}")
endfunction()

# One target for consumers to link, so the three glslang archives -- the
# compiler, the SPIR-V back end and the default TBuiltInResource -- stay an
# implementation detail of this module.
if (NOT TARGET eacp-glslang)
    eacp_add_glslang()

    add_library(eacp-glslang INTERFACE)
    target_link_libraries(eacp-glslang INTERFACE
            glslang::glslang
            glslang::SPIRV
            glslang::glslang-default-resource-limits)
endif ()
