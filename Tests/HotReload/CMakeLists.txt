# Windows dev mode end to end, through eacp_add_hot_reload itself: an app whose
# export list the module links against, a module that calls into the app, into
# eacp-core and into a system library of its own (winmm), and reloads driven by
# rewriting the module's source. The source the app watches is a copy in the
# build directory, so the test rewrites that and never the tree. The test builds
# the module with `cmake --build` of this build directory, in the compiler
# environment configure ran in, which the replay (eacp-hot-reload-build.env)
# provides.
set(module_template "${CMAKE_CURRENT_SOURCE_DIR}/EndToEndModule.cpp")
set(module_source "${CMAKE_CURRENT_BINARY_DIR}/EndToEndModule.cpp")
configure_file(EndToEndModule.cpp "${module_source}" COPYONLY)

add_executable(HotReloadEndToEnd EndToEndApp.cpp)
target_link_libraries(HotReloadEndToEnd PRIVATE eacp-core)
set_default_warnings_level(HotReloadEndToEnd)

# A file name unlike the target's, so the export list must name the image the
# loader will match to the running exe.
set_target_properties(HotReloadEndToEnd PROPERTIES OUTPUT_NAME HotReloadEndToEndApp)

eacp_add_hot_reload(HotReloadEndToEnd HotReloadEndToEndModule
        SOURCES "${module_source}"
        HOT_RELOAD ON)
target_link_libraries(HotReloadEndToEndModule PRIVATE eacp-core winmm)
target_include_directories(HotReloadEndToEndModule PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

# A test killed mid-edit leaves a broken copy behind; every build of the module
# puts the template back first, except the test's own.
add_custom_target(HotReloadEndToEndModuleSource
        COMMAND "${CMAKE_COMMAND}" "-DTEMPLATE=${module_template}"
                "-DCOPY=${module_source}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/RestoreModuleSource.cmake"
        VERBATIM)
add_dependencies(HotReloadEndToEndModule HotReloadEndToEndModuleSource)

get_target_property(dev_mode HotReloadEndToEnd EACP_HOT_RELOAD_ENABLED)

if (dev_mode)
    target_compile_definitions(HotReloadEndToEnd PRIVATE
            EACP_END_TO_END_MODULE_TEMPLATE="${module_template}"
            EACP_END_TO_END_MODULE_SOURCE="${module_source}")

    add_test(NAME HotReloadEndToEnd COMMAND HotReloadEndToEnd)
    set_tests_properties(HotReloadEndToEnd PROPERTIES TIMEOUT 900)
    return()
endif ()

# CI configures every Windows lane for dev mode, so a static app there means the
# path this test covers is not being built at all.
if (EACP_CI_BUILD)
    message(FATAL_ERROR "HotReloadEndToEnd is in static mode in a CI build; "
            "Windows CI must configure for hot reload (see the configure line "
            "printed for it above)")
endif ()

set_target_properties(HotReloadEndToEnd HotReloadEndToEndModule PROPERTIES
        EXCLUDE_FROM_ALL ON)

# Reported as skipped rather than left out, so a static build directory says
# that this check did not run.
set(skip_script "${CMAKE_CURRENT_BINARY_DIR}/SkipEndToEnd.cmake")
file(WRITE "${skip_script}"
        "message(\"HotReloadEndToEnd: skipped, the app is in static mode \"\n"
        "        \"(the configure line for it says why)\")\n"
        "cmake_language(EXIT 77)\n")
add_test(NAME HotReloadEndToEnd COMMAND "${CMAKE_COMMAND}" -P "${skip_script}")
set_tests_properties(HotReloadEndToEnd PROPERTIES SKIP_RETURN_CODE 77)
