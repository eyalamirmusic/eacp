# eacp_webview_publish_generated — atomically publish a freshly rendered file
# into OUTPUT, safely even when several build directories share the same
# (source-tree) OUTPUT path.
#
# Why this exists: the WebView generators render configure-time TS into the
# *source* tree (e.g. <app>/src/generated), a path derived from the source dir
# and therefore identical across every build directory. configure_file writes
# via a fixed "<output>.tmp" sitting next to OUTPUT; when two build dirs
# configure the same source-tree OUTPUT concurrently (e.g. CLion reloading the
# Debug and Release profiles together) their temps collide and one run dies
# with a raw "No such file or directory".
#
# The fix: callers render into the per-build-dir binary tree (never shared),
# then hand the staged file here. We rename it into place — an atomic operation
# that two concurrent runs can both perform without ENOENT (last writer wins,
# with identical content). The rename is skipped when content is unchanged, so
# OUTPUT's mtime doesn't bump on every reconfigure (which would otherwise
# re-trigger the vite build through its CONFIGURE_DEPENDS glob).
function(eacp_webview_publish_generated STAGED OUTPUT)
    if (EXISTS "${OUTPUT}")
        file(SHA256 "${STAGED}" stagedHash)
        file(SHA256 "${OUTPUT}" outputHash)
        if (stagedHash STREQUAL outputHash)
            file(REMOVE "${STAGED}")
            return()
        endif ()
    endif ()

    get_filename_component(outDir "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${outDir}")
    file(RENAME "${STAGED}" "${OUTPUT}")
endfunction()
