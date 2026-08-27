set(EACP_PCH_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(EACP_PCH "Precompile the system headers every eacp target parses" OFF)

# Opt a target out. MSVC compares the conformance and codegen flags an image was
# created under against every translation unit that loads it and fails outright
# on a mismatch (C2855), so a target compiled differently has to go without.
function(eacp_skip_pch target)
    set_target_properties(${target} PROPERTIES EACP_SKIP_PCH ON)
endfunction()

# An image is shared by every target that can use it, through REUSE_FROM. A
# per-target PCH would be created once per app, test and plugin -- around 150
# times -- and creating one costs more than the handful of translation units in
# a typical app target could ever save.
function(eacp_create_pch_image name)
    # One source per language a consumer might compile: CMake builds a separate
    # image for each, and a consumer whose language the owner lacks would get a
    # /Yu with no /Yc behind it.
    set(sources "${EACP_PCH_DIR}/Pch.cpp" "${EACP_PCH_DIR}/Pch.c")

    if (APPLE)
        list(APPEND sources "${EACP_PCH_DIR}/Pch.mm")
    endif ()

    add_library(${name} STATIC ${sources})
    eacp_skip_pch(${name})

    # Miro exports /Zc:preprocessor to everything that links it, which is nearly
    # every target here, so the image has to be created under it too.
    # COMPILE_ONLY takes the usage requirements without the link edge -- the
    # image must not queue behind Miro when 600-odd translation units are
    # waiting on it.
    if (TARGET Miro)
        target_link_libraries(${name} PRIVATE $<COMPILE_ONLY:Miro>)
    endif ()

    target_precompile_headers(${name} PRIVATE "${EACP_PCH_DIR}/Pch.h")
endfunction()

# MSVC bakes the warning level into an image and lets it override the level the
# consumer was compiled with -- in both directions. eacp's libraries and apps
# are at /W4 and the NanoTest-built executables at the default level, so a
# single image would silently raise one group or silence the other. One image
# per level keeps every target reporting exactly what it reported before.
#
# C4652 is deliberately left unsuppressed: it is the diagnostic that fires when
# a target lands on the wrong image, and silencing it would hide the warning
# level changing under a target rather than fix it.
function(eacp_target_is_w4 target out)
    set(${out} FALSE PARENT_SCOPE)

    get_target_property(options ${target} COMPILE_OPTIONS)

    if (options MATCHES "/W4")
        set(${out} TRUE PARENT_SCOPE)
        return ()
    endif ()

    # /W4 also arrives through a dependency's usage requirements -- Miro ships
    # an interface target carrying it, which its generated codegen hosts link.
    get_target_property(libraries ${target} LINK_LIBRARIES)

    foreach (library IN LISTS libraries)
        if (TARGET ${library})
            get_target_property(inherited ${library} INTERFACE_COMPILE_OPTIONS)

            if (inherited MATCHES "/W4")
                set(${out} TRUE PARENT_SCOPE)
                return ()
            endif ()
        endif ()
    endforeach ()
endfunction()

# Clang stamps the PIE level into an image and rejects any translation unit
# whose level differs ("is pie differs in PCH file vs. current file"). eacp
# compiles position-independent throughout, and CMake spells that -fPIC for a
# library and -fPIE for an executable, so the two groups cannot share an image.
# MSVC draws no such distinction, and Apple builds green on a single image, so
# the split is confined to the drivers that act on the flag.
function(eacp_needs_pie_image out)
    set(${out} FALSE PARENT_SCOPE)

    if (MSVC OR APPLE OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        return ()
    endif ()

    if (CMAKE_CXX_COMPILE_OPTIONS_PIE AND NOT
            CMAKE_CXX_COMPILE_OPTIONS_PIE STREQUAL CMAKE_CXX_COMPILE_OPTIONS_PIC)
        set(${out} TRUE PARENT_SCOPE)
    endif ()
endfunction()

# Nothing links the image -- REUSE_FROM shares the precompiled header alone --
# so the flag is here to match what the consumers compile under rather than to
# change the code generated for it. The property goes off first so CMake does
# not emit its own -fPIC and leave which of the two wins up to flag order.
function(eacp_make_image_pie name)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${name} PRIVATE ${CMAKE_CXX_COMPILE_OPTIONS_PIE})
endfunction()

function(eacp_pch_image_for target out)
    set(${out} eacp-pch PARENT_SCOPE)

    if (MSVC)
        eacp_target_is_w4(${target} is_w4)

        if (is_w4)
            set(${out} eacp-pch-w4 PARENT_SCOPE)
        endif ()

        return ()
    endif ()

    # The image exists only where the PIE level was worth splitting on, so its
    # presence is the condition -- no second reading of what eacp_apply_pch
    # already decided.
    if (TARGET eacp-pch-pie)
        get_target_property(type ${target} TYPE)

        if (type STREQUAL "EXECUTABLE")
            set(${out} eacp-pch-pie PARENT_SCOPE)
        endif ()
    endif ()
endfunction()

function(eacp_apply_pch_in_directory dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)

    foreach (target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        get_target_property(skip ${target} EACP_SKIP_PCH)

        if (skip OR NOT type MATCHES
                "^(STATIC|SHARED|MODULE|OBJECT)_LIBRARY$|^EXECUTABLE$")
            continue ()
        endif ()

        eacp_pch_image_for(${target} image)
        target_precompile_headers(${target} REUSE_FROM ${image})

        if (MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # An image serves targets that each carry their own -D -- a harness
            # path for a test, a plugin's export symbol -- and clang-cl reports
            # every one as a mismatch against it. It cannot tell an inert
            # difference from a real one. What makes them inert here is that the
            # image holds system headers only, so a macro in eacp's own
            # namespace cannot change what is in it. A define that would change
            # it -- NOMINMAX, _WIN32_WINNT, _HAS_EXCEPTIONS -- belongs in Pch.h
            # itself, or the target setting it belongs in eacp_skip_pch.
            target_compile_options(${target} PRIVATE -Wno-clang-cl-pch)
        endif ()
    endforeach ()

    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)

    foreach (subdir IN LISTS subdirs)
        get_property(subdir_binary DIRECTORY "${subdir}" PROPERTY BINARY_DIR)
        string(FIND "${subdir_binary}" "${dependency_dir}/" is_a_dependency)

        # CPM adds each package as a subdirectory of whichever eacp directory
        # asked for it, so the walk reaches them; they build on their own flags.
        # Matching on the binary dir rather than the source dir is what makes
        # this hold when CPM_SOURCE_CACHE points inside the repository.
        if (NOT is_a_dependency EQUAL 0)
            eacp_apply_pch_in_directory("${subdir}")
        endif ()
    endforeach ()
endfunction()

function(eacp_apply_pch)
    if (NOT EACP_PCH)
        return ()
    endif ()

    set(dependency_dir "${FETCHCONTENT_BASE_DIR}")

    if (NOT dependency_dir)
        set(dependency_dir "${eacp_BINARY_DIR}/_deps")
    endif ()

    # Created here rather than at the top of the project so Miro already exists
    # and can hand over the flags its consumers compile under.
    eacp_create_pch_image(eacp-pch)

    if (MSVC)
        eacp_create_pch_image(eacp-pch-w4)
        set_default_warnings_level(eacp-pch-w4)
    endif ()

    eacp_needs_pie_image(needs_pie)

    if (needs_pie)
        eacp_create_pch_image(eacp-pch-pie)
        eacp_make_image_pie(eacp-pch-pie)
    endif ()

    eacp_apply_pch_in_directory("${eacp_SOURCE_DIR}")
endfunction()
