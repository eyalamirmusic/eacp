include(CPM)

# glslang stamps its own FOLDER and turns on its own -Wall: put its targets with
# the other CPM deps, and silence them, so a build log carries eacp's warnings
# alone. The walk recurses because glslang spreads its targets over subdirectories.
function(eacp_adopt_glslang_targets dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)

    foreach (target IN LISTS targets)
        set_target_properties(${target} PROPERTIES FOLDER "${CMAKE_FOLDER}")
        silence_target_warnings(${target})
    endforeach ()

    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)

    foreach (subdir IN LISTS subdirs)
        eacp_adopt_glslang_targets("${subdir}")
    endforeach ()
endfunction()

# Everything that would drag in a second dependency tree is off. In a function
# so nothing leaks past the fetch; CMP0077 is NEW, so these beat its option()s.
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

    # glslang defaults the other way, and on MSVC /D_HAS_EXCEPTIONS=0 changes
    # the layout of the std::string and std::vector its API passes across.
    set(ENABLE_EXCEPTIONS ON)
    set(ENABLE_RTTI ON)

    CPMAddPackage(
            NAME glslang
            GITHUB_REPOSITORY KhronosGroup/glslang
            GIT_TAG 16.5.0
            GIT_SHALLOW YES
            SYSTEM YES)

    eacp_adopt_glslang_targets("${glslang_SOURCE_DIR}")
endfunction()

if (NOT TARGET eacp-glslang)
    eacp_add_glslang()

    add_library(eacp-glslang INTERFACE)
    target_link_libraries(eacp-glslang INTERFACE
            glslang::glslang
            glslang::SPIRV
            glslang::glslang-default-resource-limits)
endif ()
