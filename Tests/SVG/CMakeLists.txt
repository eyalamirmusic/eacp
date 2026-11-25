nano_add_executable(SVGTests
        SOURCES
            SVGClipTests.cpp
            SVGGeometryTests.cpp
            SVGGradientTests.cpp
            SVGOpacityTests.cpp
        TARGETS eacp-svg)

# Its own executable because this one needs a GPU device, which the suite above
# deliberately does not, and a main that gives it an app to run inside.
nano_add_executable(SVGImageTests
        NO_MAIN
        SOURCES
            SVGImageElementTests.cpp
            SVGImageTests.cpp
            TestMain.cpp
        TARGETS eacp-svg)

# On the web that device is WebGPU, so the suite runs in headless Chrome as
# GPUTests does, one page per group.
if (EMSCRIPTEN)
    eacp_web_link_options(svg_image_web_link_options)
    target_link_options(SVGImageTests PRIVATE ${svg_image_web_link_options})
    target_link_libraries(SVGImageTests PRIVATE eacp-core-nested-pump)
    set_target_properties(SVGImageTests PROPERTIES EACP_WEB_NESTED_PUMP TRUE)
    eacp_web_discover_tests(SVGImageTests HEADLESS)
endif ()
