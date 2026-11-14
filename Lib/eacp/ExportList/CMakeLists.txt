# Windows only: the COFF reader, the export filter and the .def writer, as a
# library the tests link, and eacp-export-list-tool, the build-time host tool
# eacp_add_export_list (CMake/EacpExportList.cmake) runs. README.md beside this
# file is the account.
add_library(eacp-export-list STATIC
        Archive.cpp
        Coff.cpp
        ExportList.cpp
        Undecorate-Windows.cpp)

target_link_libraries(eacp-export-list PUBLIC eacp-core)
target_link_libraries(eacp-export-list PRIVATE dbghelp)
set_default_warnings_level(eacp-export-list)

# The tool reruns whenever a closure archive changes, so it reads its inputs at
# full speed in every configuration, Debug included.
eacp_force_optimization(eacp-export-list)
add_ide_sources(eacp-export-list)

add_executable(eacp-export-list-tool Main.cpp)
target_link_libraries(eacp-export-list-tool PRIVATE eacp-export-list)
set_default_warnings_level(eacp-export-list-tool)
eacp_force_optimization(eacp-export-list-tool)
