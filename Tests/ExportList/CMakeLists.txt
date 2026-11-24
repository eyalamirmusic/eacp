# The export list's inputs are compiled here, by the build's own compiler and
# librarian, rather than checked in: one source as a classic object, as a
# /bigobj one and under /Gy, an archive with a long member name, and the same
# source under link-time optimisation, loose and archived.
set(fixtures "${CMAKE_CURRENT_LIST_DIR}/Fixtures")

function(eacp_export_list_fixture target type)
    add_library(${target} ${type} ${ARGN})
    set_target_properties(${target} PROPERTIES
            UNITY_BUILD OFF
            INTERPROCEDURAL_OPTIMIZATION OFF)
endfunction()

eacp_export_list_fixture(ExportListFixtureClassic OBJECT "${fixtures}/Symbols.cpp")
eacp_export_list_fixture(ExportListFixtureBigObj OBJECT "${fixtures}/Symbols.cpp")
target_compile_options(ExportListFixtureBigObj PRIVATE /bigobj)
eacp_export_list_fixture(ExportListFixtureGy OBJECT "${fixtures}/Symbols.cpp")
target_compile_options(ExportListFixtureGy PRIVATE /Gy)

eacp_export_list_fixture(ExportListFixtureArchive STATIC
        "${fixtures}/Symbols.cpp"
        "${fixtures}/ArchiveMemberWithALongFileName.cpp")

# clang-cl has no /GL; -flto is its link-time optimisation, and writes bitcode.
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(link_time_code_option /GL)
else ()
    set(link_time_code_option -flto)
endif ()

eacp_export_list_fixture(ExportListFixtureLinkTimeCode OBJECT
        "${fixtures}/LinkTimeCode.cpp")
eacp_export_list_fixture(ExportListFixtureLinkTimeCodeArchive STATIC
        "${fixtures}/ArchiveMemberWithALongFileName.cpp"
        "${fixtures}/LinkTimeCode.cpp")
set_source_files_properties("${fixtures}/LinkTimeCode.cpp" PROPERTIES
        COMPILE_OPTIONS ${link_time_code_option})

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_target_properties(ExportListFixtureLinkTimeCodeArchive PROPERTIES
            STATIC_LIBRARY_OPTIONS /LTCG)
endif ()

set(fixture_targets
        ExportListFixtureClassic
        ExportListFixtureBigObj
        ExportListFixtureGy
        ExportListFixtureArchive
        ExportListFixtureLinkTimeCode
        ExportListFixtureLinkTimeCodeArchive)

nano_add_executable(ExportListTests
        SOURCES ExportListTests.cpp ExportListToolTests.cpp
        TARGETS eacp-export-list)

target_compile_definitions(ExportListTests PRIVATE
        EXPORT_LIST_CLASSIC="$<TARGET_OBJECTS:ExportListFixtureClassic>"
        EXPORT_LIST_BIGOBJ="$<TARGET_OBJECTS:ExportListFixtureBigObj>"
        EXPORT_LIST_GY="$<TARGET_OBJECTS:ExportListFixtureGy>"
        EXPORT_LIST_ARCHIVE="$<TARGET_FILE:ExportListFixtureArchive>"
        EXPORT_LIST_LINK_TIME_CODE="$<TARGET_OBJECTS:ExportListFixtureLinkTimeCode>"
        EXPORT_LIST_LINK_TIME_CODE_ARCHIVE="$<TARGET_FILE:ExportListFixtureLinkTimeCodeArchive>"
        EXPORT_LIST_TOOL="$<TARGET_FILE:eacp-export-list-tool>")

# A thin archive needs llvm-lib, the librarian of a clang-cl build, which a
# Visual Studio install carries beside cl as well.
get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(EACP_LLVM_LIB llvm-lib
        HINTS "${compiler_dir}"
              "${compiler_dir}/../../../../../Llvm/x64/bin"
              "${compiler_dir}/../../../../../Llvm/ARM64/bin"
        NO_DEFAULT_PATH)

if (EACP_LLVM_LIB)
    set(thin_archive "${CMAKE_CURRENT_BINARY_DIR}/ExportListFixtureThin.lib")

    add_custom_command(
            OUTPUT "${thin_archive}"
            COMMAND "${EACP_LLVM_LIB}" /nologo /llvmlibthin "/out:${thin_archive}"
                    $<TARGET_OBJECTS:ExportListFixtureClassic>
            DEPENDS ExportListFixtureClassic $<TARGET_OBJECTS:ExportListFixtureClassic>
            COMMENT "Making a thin archive for ExportListTests"
            VERBATIM)

    add_custom_target(ExportListFixtureThin DEPENDS "${thin_archive}")
    list(APPEND fixture_targets ExportListFixtureThin)
    target_compile_definitions(ExportListTests PRIVATE
            EXPORT_LIST_THIN_ARCHIVE="${thin_archive}")
endif ()

# eacp_add_export_list end to end, as hot reload uses it: a host executable
# exports the fixture archive through the list, and a module linked against
# nothing but the import library made from that list runs inside the host.
set(fixture_def "${CMAKE_CURRENT_BINARY_DIR}/ExportListFixtureHost.def")
set(fixture_imports "${CMAKE_CURRENT_BINARY_DIR}/ExportListFixtureHost-imports.lib")

eacp_add_export_list(ExportListFixtureExports
        DEF "${fixture_def}"
        IMAGE ExportListFixtureHost.exe
        TARGETS ExportListFixtureArchive
        IMPORT_LIBRARY "${fixture_imports}")

add_executable(ExportListFixtureHost "${fixtures}/Host.cpp")
target_include_directories(ExportListFixtureHost PRIVATE
        $<TARGET_PROPERTY:eacp-core,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(ExportListFixtureHost PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,ExportListFixtureArchive>")
target_link_options(ExportListFixtureHost PRIVATE "/DEF:${fixture_def}")
set_property(TARGET ExportListFixtureHost APPEND PROPERTY LINK_DEPENDS "${fixture_def}")

add_library(ExportListFixtureModule MODULE "${fixtures}/Module.cpp")
target_link_libraries(ExportListFixtureModule PRIVATE "${fixture_imports}")
set_property(TARGET ExportListFixtureModule APPEND PROPERTY
        LINK_DEPENDS "${fixture_imports}")

foreach (target ExportListFixtureHost ExportListFixtureModule)
    set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)
    add_dependencies(${target} ExportListFixtureExports)
endforeach ()

list(APPEND fixture_targets ExportListFixtureHost ExportListFixtureModule)
target_compile_definitions(ExportListTests PRIVATE
        EXPORT_LIST_FIXTURE_DEF="${fixture_def}"
        EXPORT_LIST_FIXTURE_HOST="$<TARGET_FILE:ExportListFixtureHost>"
        EXPORT_LIST_FIXTURE_MODULE="$<TARGET_FILE:ExportListFixtureModule>")

add_dependencies(ExportListTests ${fixture_targets} eacp-export-list-tool)
