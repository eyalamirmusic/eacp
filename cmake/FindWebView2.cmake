include(FindPackageHandleStandardArgs)
include(FetchContent)

set(WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "WebView2 NuGet package version")

# Detect target architecture for WebView2 library selection
# Use CMAKE_CXX_COMPILER_ARCHITECTURE_ID which reflects the actual target
# This handles cross-compilation scenarios correctly
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _target_arch)

if(_target_arch MATCHES "arm64|aarch64")
    set(WebView2_arch arm64)
elseif(_target_arch MATCHES "x64|amd64")
    set(WebView2_arch x64)
elseif(_target_arch MATCHES "x86|i386|i686")
    set(WebView2_arch x86)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Fallback for non-MSVC compilers
    set(WebView2_arch x64)
else()
    set(WebView2_arch x86)
endif()

message(STATUS "WebView2 target architecture: ${WebView2_arch} (compiler arch ID: ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID})")

set(local_nuget_dir "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages")
file(GLOB subdirs "${local_nuget_dir}/*Microsoft.Web.WebView2*")

if(subdirs)
    list(GET subdirs 0 WebView2_root_dir)
    message(STATUS "Found WebView2 in local NuGet cache: ${WebView2_root_dir}")
else()
    set(WebView2_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/webview2")
    set(WebView2_root_dir "${WebView2_DOWNLOAD_DIR}/Microsoft.Web.WebView2.${WEBVIEW2_VERSION}")

    if(NOT EXISTS "${WebView2_root_dir}/build/native/include/WebView2.h")
        message(STATUS "Downloading WebView2 NuGet package...")

        set(WEBVIEW2_URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}")

        file(DOWNLOAD
            ${WEBVIEW2_URL}
            "${WebView2_DOWNLOAD_DIR}/webview2.zip"
            STATUS download_status
            SHOW_PROGRESS
        )

        list(GET download_status 0 status_code)
        if(NOT status_code EQUAL 0)
            message(FATAL_ERROR "Failed to download WebView2 package")
        endif()

        file(ARCHIVE_EXTRACT
            INPUT "${WebView2_DOWNLOAD_DIR}/webview2.zip"
            DESTINATION "${WebView2_root_dir}"
        )

        message(STATUS "WebView2 extracted to: ${WebView2_root_dir}")
    endif()
endif()

set(WebView2_include_dir "${WebView2_root_dir}/build/native/include")
set(WebView2_library "${WebView2_root_dir}/build/native/${WebView2_arch}/WebView2LoaderStatic.lib")

find_package_handle_standard_args(WebView2 DEFAULT_MSG WebView2_include_dir WebView2_library)

if(WebView2_FOUND)
    set(WebView2_INCLUDE_DIRS ${WebView2_include_dir})
    set(WebView2_LIBRARIES ${WebView2_library})

    mark_as_advanced(WebView2_library WebView2_include_dir WebView2_root_dir)

    if(NOT TARGET WebView2::WebView2)
        add_library(WebView2::WebView2 INTERFACE IMPORTED)
        set_target_properties(WebView2::WebView2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${WebView2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${WebView2_LIBRARIES}"
        )
    endif()
endif()
