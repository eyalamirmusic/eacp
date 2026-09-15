# Configure-time output. A clean configure should read as the list of
# dependencies and the three lines that close it, not narrate every sub-build
# FetchContent spawns and every library pkg-config is asked about. CPM's own
# line per package stays as it comes: it names where the source came from, at
# which tag, and any local override pointed somewhere else, which is the part
# worth reading.
#
# EACP_VERBOSE_CONFIGURE puts the rest back. It raises the message log level
# rather than switching each site on individually, because the log level is the
# one knob CMake, FetchContent and eacp all already read. Warnings and errors
# are above it and print either way.

option(EACP_VERBOSE_CONFIGURE
        "Print the third-party fetch and probe detail hidden by default" OFF)

if (EACP_VERBOSE_CONFIGURE)
    set(CMAKE_MESSAGE_LOG_LEVEL VERBOSE)
else ()
    # The check_* probes inside FindThreads and its kind: a "Performing Test"
    # pair each, for a result nothing reads unless the configure fails on it.
    set(CMAKE_REQUIRED_QUIET ON)

    # Miro asks for Threads without QUIET, and eacp cannot pass a flag into
    # somebody else's find_package call. QUIET reaches the find module as this
    # variable, and find_package leaves one already set alone, so setting it
    # here is the same instruction from the outside. A REQUIRED package that is
    # missing still fails the configure: quiet covers the report, not the
    # result.
    set(Threads_FIND_QUIETLY TRUE)
endif ()

# Handed to the find_package and pkg_check_modules calls whose absence eacp
# already reports itself, with a better message than the finder's.
set(EACP_FIND_QUIET QUIET)

if (EACP_VERBOSE_CONFIGURE)
    set(EACP_FIND_QUIET "")
endif ()

# Handed to the CPMAddPackage calls that pass DOWNLOAD_ONLY. Those are the ones
# CPM populates through FetchContent_Populate's explicit-details form, which
# takes its own QUIET and ignores FETCHCONTENT_QUIET - so without this each one
# prints a sub-build configure report and nine build lines. The packages that
# are added as subdirectories go through FetchContent_MakeAvailable, which
# FETCHCONTENT_QUIET already covers. CPM hashes a package's arguments into its
# CPM_SOURCE_CACHE directory name, so flipping this re-downloads those three
# once for anyone who keeps such a cache; nothing here does.
set(EACP_FETCH_QUIET QUIET)

if (EACP_VERBOSE_CONFIGURE)
    set(EACP_FETCH_QUIET "")
endif ()
