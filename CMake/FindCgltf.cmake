include(CPM)

# cgltf is a single header with no CMakeLists of its own, so the fetch is
# DOWNLOAD_ONLY and the target is declared here.
CPMAddPackage(
        NAME Cgltf
        GITHUB_REPOSITORY jkuhlmann/cgltf
        GIT_TAG v1.15
        DOWNLOAD_ONLY YES)

if (Cgltf_ADDED AND NOT TARGET cgltf)
    # The header carries its own implementation behind CGLTF_IMPLEMENTATION,
    # which has to be defined in exactly one translation unit. That unit is
    # generated here rather than checked into Lib/eacp/Mesh so cgltf's code is
    # never compiled under eacp's own warning level - it does not build clean
    # under -Wall -Wextra -Wpedantic, and relaxing those for the module that
    # includes it would relax them for the module's own sources too.
    set(cgltf_implementation "${CMAKE_CURRENT_BINARY_DIR}/cgltf_implementation.c")

    file(WRITE "${cgltf_implementation}"
            "#define CGLTF_IMPLEMENTATION\n#include <cgltf.h>\n")

    add_library(cgltf STATIC "${cgltf_implementation}")

    # SYSTEM for the same reason the implementation is generated: a consumer
    # including cgltf.h should not inherit its warnings.
    target_include_directories(cgltf SYSTEM PUBLIC "${Cgltf_SOURCE_DIR}")

    set_target_properties(cgltf PROPERTIES FOLDER Dependencies)

    add_library(cgltf::cgltf ALIAS cgltf)
endif ()
