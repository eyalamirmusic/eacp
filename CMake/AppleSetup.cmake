macro(eacp_setup_apple)
    if (IOS)
        set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "LK9GL8NWU4"
                CACHE STRING "" FORCE)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
                CACHE STRING "" FORCE)
        # CACHE INTERNAL so the path survives this macro's expansion inside the
        # eacp_default_setup() function — a plain set() would die with that
        # function scope and never reach set_default_target_setting(), leaving
        # every bundle on CMake's default Info.plist (no iOS launch screen or
        # scene manifest).
        set(EACP_IOS_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/CMake/iOSBundleInfo.plist.in"
                CACHE INTERNAL "eacp iOS bundle Info.plist template")
    else ()
        # CMake's Darwin module already creates CMAKE_OSX_DEPLOYMENT_TARGET as an
        # empty cache entry during project(), so a plain `set(... CACHE STRING "")`
        # here is a no-op and every binary silently inherits the build machine's
        # SDK version. Claim the entry only when nothing has filled it in, which
        # still lets -DCMAKE_OSX_DEPLOYMENT_TARGET=... on the command line win.
        if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
            set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
                    "Minimum macOS version eacp targets" FORCE)
        endif ()

        set(EACP_MACOS_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/CMake/macOSBundleInfo.plist.in"
                CACHE INTERNAL "eacp macOS bundle Info.plist template")
    endif ()
endmacro()
