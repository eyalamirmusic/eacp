# The iOS signing defaults are applied at the end of the top-level directory,
# and only to what the project left unset there. The Xcode generator reads the
# CMAKE_XCODE_ATTRIBUTE_* variables from that scope, so this is the one place
# where every way a project can name its own team is already visible: on the
# command line, or as a normal variable or a cache entry set before or after
# eacp's setup ran. A project that names a team, an empty one included, keeps
# it; one that names none signs with EACP_APPLE_DEVELOPMENT_TEAM.
macro(_eacp_apply_apple_signing_defaults)
    if (NOT DEFINED CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
            AND EACP_APPLE_DEVELOPMENT_TEAM)
        set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
                "${EACP_APPLE_DEVELOPMENT_TEAM}")
    endif ()

    if (NOT DEFINED CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY)
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer")
    endif ()
endmacro()

macro(eacp_setup_apple)
    # CMake's Darwin module already creates CMAKE_OSX_DEPLOYMENT_TARGET as an
    # empty cache entry during project(), so a plain `set(... CACHE STRING "")`
    # here is a no-op and every binary silently inherits the build machine's
    # SDK version. Claim the entry only when nothing has filled it in, which
    # still lets -DCMAKE_OSX_DEPLOYMENT_TARGET=... on the command line win.
    if (IOS)
        set(EACP_APPLE_DEVELOPMENT_TEAM "LK9GL8NWU4" CACHE STRING
                "Apple development team eacp's iOS targets sign with when the project sets no CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM; empty for none")

        get_property(_eacp_signing_deferred GLOBAL
                PROPERTY EACP_APPLE_SIGNING_DEFAULTS_DEFERRED)

        if (NOT _eacp_signing_deferred)
            set_property(GLOBAL
                    PROPERTY EACP_APPLE_SIGNING_DEFAULTS_DEFERRED TRUE)
            cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                    CALL _eacp_apply_apple_signing_defaults)
        endif ()

        unset(_eacp_signing_deferred)

        if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
            set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING
                    "Minimum iOS version eacp targets" FORCE)
        endif ()
    else ()
        if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
            set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
                    "Minimum macOS version eacp targets" FORCE)
        endif ()
    endif ()
endmacro()
