include(CPM)

set(_eadatastructures_patch
        "${CMAKE_CURRENT_LIST_DIR}/patches/eadatastructures-cxx23-aligned-storage.patch")

CPMAddPackage(
        NAME EADataStructures
        GITHUB_REPOSITORY eyalamirmusic/cpp_data_structures
        GIT_TAG main)

set(_eadatastructures_probe
        "${EADataStructures_SOURCE_DIR}/ea_data_structures/Structures/FixedDynamicArray.h")

if (EXISTS "${_eadatastructures_probe}")
    file(READ "${_eadatastructures_probe}" _eadatastructures_probe_contents)
    if (_eadatastructures_probe_contents MATCHES "aligned_storage_t")
        find_program(_eadatastructures_patch_executable patch REQUIRED)
        execute_process(
                COMMAND "${_eadatastructures_patch_executable}" -p1 -i "${_eadatastructures_patch}"
                WORKING_DIRECTORY "${EADataStructures_SOURCE_DIR}"
                RESULT_VARIABLE _eadatastructures_patch_result
                OUTPUT_VARIABLE _eadatastructures_patch_output
                ERROR_VARIABLE _eadatastructures_patch_error)
        if (NOT _eadatastructures_patch_result EQUAL 0)
            message(FATAL_ERROR
                    "Failed to patch EADataStructures for C++23 aligned storage deprecations:\n"
                    "${_eadatastructures_patch_output}\n${_eadatastructures_patch_error}")
        endif ()
    endif ()
endif ()
