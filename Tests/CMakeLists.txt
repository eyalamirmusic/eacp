set(CMAKE_FOLDER Tests)

find_package(NanoTest REQUIRED)

set(NANO_TEST_GROUP_BY PREFIX)
set(CMAKE_UNITY_BUILD ${EACP_UNITY_BUILD_TESTS})

# The hand-written shaders a suite carries, one file per dialect, embedded and
# keyed by their path under DIRECTORY (default: the suite's Shaders/), which
# TestShaders.h reads back. FILES embeds those paths alone, for a suite that
# shares a few of another's.
function(eacp_embed_test_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DIRECTORY" "FILES")

    if (NOT ARG_DIRECTORY)
        set(ARG_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
    endif ()

    if (ARG_FILES)
        list(TRANSFORM ARG_FILES PREPEND "${ARG_DIRECTORY}/" OUTPUT_VARIABLE shaders)
    else ()
        file(GLOB_RECURSE shaders CONFIGURE_DEPENDS
                "${ARG_DIRECTORY}/*.metal"
                "${ARG_DIRECTORY}/*.hlsl"
                "${ARG_DIRECTORY}/*.glsl"
                "${ARG_DIRECTORY}/*.wgsl")
    endif ()

    res_embed_add(${target}
            FILES ${shaders}
            BASE_DIRECTORY "${ARG_DIRECTORY}"
            NAMESPACE TestShaders
            CATEGORY TestShaders
            TU_COUNT 8)
endfunction()

add_subdirectory(Core)

if (APPLE)
    add_subdirectory(ObjC)
endif ()

# Configure-time checks of the Apple setup; they need Xcode, not a target.
if (CMAKE_HOST_APPLE)
    add_subdirectory(AppleSetup)
endif ()

add_subdirectory(Network)

if (TARGET eacp-export-list)
    add_subdirectory(ExportList)
endif ()

# Windows dev mode through eacp_add_hot_reload itself; macOS and Linux are
# covered by the lower layers' suites and the in-tree examples.
if (WIN32)
    add_subdirectory(HotReload)
endif ()

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

# One ctest entry per case group, through node, for every suite the web builds,
# in place of the one whole-suite entry each, which ctest is told to ignore.
# The suites wait in nested pumps (runEventLoopUntil and what is built on it),
# so they link the JSPI half of the loop, and run on node's file system.
if (EMSCRIPTEN)
    eacp_web_link_options(node_link_options NODE_FILESYSTEM)
    set(web_suites "")

    foreach (suite CoreTests NetworkTests GPUCodegenTests CpuComputeTests
            MLGraphTests ScriptHostTests GraphicsTests GPUWidgetsTests)
        if (TARGET ${suite})
            target_link_options(${suite} PRIVATE ${node_link_options})
            target_link_libraries(${suite} PRIVATE eacp-core-nested-pump)
            eacp_web_discover_tests(${suite})
            list(APPEND web_suites ${suite})
        endif ()
    endforeach ()

    # Discovered by Tests/GPU itself, into headless Chrome rather than node.
    if (TARGET GPUTests)
        list(APPEND web_suites GPUTests)
    endif ()

    file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
            "set(CTEST_CUSTOM_TESTS_IGNORE ${web_suites})\n")

    if (TARGET CpuComputeBench)
        target_link_options(CpuComputeBench PRIVATE ${node_link_options})
    endif ()

    # The example apps and the quit harnesses, built without JSPI as an app
    # is. HelloNetwork in a browser shows the browser's own rules - CORS,
    # forbidden headers, opaque redirects - that the node-run cases cannot; a
    # harness checks that a real exit with work in flight stays clean. Console
    # and ZipDemo are examples of every platform, run unchanged.
    foreach (app HelloLoop HelloNetwork QuitWithTimerHarness QuitWithFetchHarness
            Console ZipDemo WebWindowHarness)
        if (TARGET ${app})
            eacp_web_add_app_tests(${app})
        endif ()
    endforeach ()

    # The network examples, which block in main() through the nested pump. Their
    # <app>-run talks to the public hosts they default to; their ctest entries
    # get the test server as a companion, whose URL they take as their last
    # argument, so the entries run offline.
    # The GPU examples that compute and exit: under node, the path a page with
    # no WebGPU takes (no device, said once); in headless Chrome, the kernels
    # on the real device, failing on any validation error it reports.
    foreach (app Compute AsyncCompute)
        if (TARGET ${app})
            eacp_web_add_app_tests(${app})
        endif ()
    endforeach ()

    foreach (app OnlineResourceDemo Http BasicHttp)
        if (TARGET ${app})
            set_target_properties(${app} PROPERTIES EACP_WEB_COMPANION
                    "${PROJECT_SOURCE_DIR}/Tests/Network/Web/TestServer.mjs")
            eacp_web_add_app_tests(${app})
        endif ()
    endforeach ()

    # A C++ exception out of a main() suspended in a nested pump must abort at
    # once with its message, under node and in a page, rather than leave the
    # runtime alive.
    if (TARGET ThrowFromMainHarness)
        set(names ThrowFromMainHarness)
        set(modes node)

        if (EACP_WEB_CHROME)
            list(APPEND names ThrowFromMainHarnessHeadless)
            list(APPEND modes headless)
        endif ()

        foreach (name mode IN ZIP_LISTS names modes)
            eacp_web_launch_command(command ThrowFromMainHarness ${mode})
            add_test(NAME ${name}
                    COMMAND ${command}
                    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ThrowFromMainHarness>")
            set_tests_properties(${name} PROPERTIES
                    PASS_REGULAR_EXPRESSION
                    "Aborted\\(uncaught exception: std::runtime_error: ThrowFromMainHarness threw on purpose\\)"
                    TIMEOUT 20)
        endforeach ()
    endif ()
endif ()
