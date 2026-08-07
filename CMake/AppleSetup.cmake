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

# The floor an app supports is the app's policy, not this library's, so eacp
# only claims CMAKE_OSX_DEPLOYMENT_TARGET for its own top-level builds and
# never reaches into a consumer's. It does have to say something when the
# consumer set no floor at all, because that is not neutral: clang then targets
# the build machine's SDK, and every @available guard whose floor that version
# already clears is folded out of the binary. The guarded call runs
# unconditionally and throws on an older system.
function(eacp_check_deployment_target)
    if (NOT APPLE OR IOS OR CMAKE_OSX_DEPLOYMENT_TARGET)
        return()
    endif ()

    message(WARNING
            "eacp: CMAKE_OSX_DEPLOYMENT_TARGET is not set, so this build takes "
            "the build machine's SDK version as its minimum macOS. eacp guards "
            "every API it calls that is newer than macOS 11, but clang removes "
            "an @available guard once the deployment target already clears it, "
            "and the call then runs unconditionally — an app built this way "
            "throws 'unrecognized selector' on an older macOS instead of taking "
            "the fallback path. Pass -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0, or "
            "whatever floor this app actually supports.")
endfunction()
