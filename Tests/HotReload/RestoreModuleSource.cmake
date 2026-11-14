# Run before every build of HotReloadEndToEndModule: puts the template back
# over the working copy when they differ, so a test that was killed mid-edit
# leaves nothing a plain rebuild cannot recover from. The test's own builds run
# with EACP_HOT_RELOAD_END_TO_END set and keep its edits. Touched after the
# copy, so it is newer than the module's object and recompiles.
#
#   cmake -DTEMPLATE=<file> -DCOPY=<file> -P RestoreModuleSource.cmake
if (DEFINED ENV{EACP_HOT_RELOAD_END_TO_END})
    return()
endif ()

file(SHA256 "${TEMPLATE}" pristine)
set(current "")

if (EXISTS "${COPY}")
    file(SHA256 "${COPY}" current)
endif ()

if (NOT current STREQUAL pristine)
    file(COPY_FILE "${TEMPLATE}" "${COPY}")
    file(TOUCH "${COPY}")
endif ()
