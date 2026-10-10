macro(eacp_setup_apple)
    # CMake's Darwin module already creates CMAKE_OSX_DEPLOYMENT_TARGET as an
    # empty cache entry during project(), so a plain `set(... CACHE STRING "")`
    # here is a no-op and every binary silently inherits the build machine's
    # SDK version. Claim the entry only when nothing has filled it in, which
    # still lets -DCMAKE_OSX_DEPLOYMENT_TARGET=... on the command line win.
    if (IOS)
        set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "LK9GL8NWU4"
                CACHE STRING "" FORCE)
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
                CACHE STRING "" FORCE)

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
