set(CMAKE_FOLDER Tests)

find_package(NanoTest REQUIRED)

set(NANO_TEST_GROUP_BY PREFIX)
set(CMAKE_UNITY_BUILD ${EACP_UNITY_BUILD_TESTS})

add_subdirectory(Core)

if (APPLE)
    add_subdirectory(ObjC)
endif ()

# Configure-time checks of the Apple setup; they need Xcode, not a target.
if (CMAKE_HOST_APPLE)
    add_subdirectory(AppleSetup)
endif ()

add_subdirectory(Network)

add_subdirectory(Spirv)

if (EACP_HAS_DRAW)
    add_subdirectory(Graphics)
endif ()

# Always added: the shader codegen tests are pure string generation, and the
# directory returns before the device suites unless EACP_HAS_GPU.
if (NOT IOS)
    add_subdirectory(GPU)
    add_subdirectory(CpuCompute)
endif ()

# Always added: the graph and MIL writer tests are bytes in and bytes out, and
# the directory returns before the Core ML suite unless EACP_HAS_COREML.
if (NOT IOS)
    add_subdirectory(ML)
endif ()

if (EACP_HAS_GPU AND NOT IOS)
    add_subdirectory(GPUWidgets)
endif ()

if (EACP_HAS_TEXT AND NOT IOS)
    add_subdirectory(Text)
    add_subdirectory(UI)

    # The parse layer and the geometry it builds. Above UI in the list because
    # SVGComponent is a UI::Component, and none of these needs a window.
    add_subdirectory(SVG)
endif ()

if (EACP_HAS_CAPTURE AND NOT IOS)
    add_subdirectory(Camera)

    # The video playback stack above the Decoder interface — frame queueing,
    # playhead-to-frame selection, the clock — is portable C++, so these run
    # against a synthetic decoder with no media file and no codec involved.
    if (NOT ANDROID)
        add_subdirectory(Video)
    endif ()
endif ()

# Codegen tests exercise eacp's WebView events/hooks format registrations,
# so they only run on the platforms where the WebView library (and its
# eacp-webview-codegen object lib) are built.
if (EACP_HAS_WEBVIEW)
    add_subdirectory(Codegen)
endif ()

# Always added: ScriptHostTests needs no web runtime and no graphics. The
# directory returns after it unless EACP_HAS_WEBVIEW.
add_subdirectory(WebView)

# eacp_default_setup() asks Clang for full LTO in Release. Read the option back
# from the directory eacp-core is generated in, so a setting that never leaves
# the function's scope fails here instead of quietly building with ThinLTO.
if (PROJECT_IS_TOP_LEVEL AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_target_property(coreSourceDirectory eacp-core SOURCE_DIR)
    get_directory_property(coreIpoOptions DIRECTORY "${coreSourceDirectory}"
            DEFINITION CMAKE_CXX_COMPILE_OPTIONS_IPO)

    add_test(NAME Build/eacpCoreTakesFullLto
            COMMAND "${CMAKE_COMMAND}" -E echo "${coreIpoOptions}")
    set_tests_properties(Build/eacpCoreTakesFullLto PROPERTIES
            PASS_REGULAR_EXPRESSION "-flto=full")
endif ()
