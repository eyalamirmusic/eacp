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
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
        set(EACP_MACOS_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/CMake/macOSBundleInfo.plist.in"
                CACHE INTERNAL "eacp macOS bundle Info.plist template")
    endif ()
endmacro()
