if (APPLE)
    enable_language(OBJCXX)
endif ()

macro(eacp_setup_apple)
    if (IOS)
        set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "LK9GL8NWU4"
                CACHE STRING "" FORCE)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
        set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
                CACHE STRING "" FORCE)
        set(EACP_IOS_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/CMake/iOSBundleInfo.plist.in")
    else ()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
        set(EACP_MACOS_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/CMake/macOSBundleInfo.plist.in")
    endif ()
endmacro()
