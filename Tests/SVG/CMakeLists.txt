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
            SVGImageTests.cpp
            TestMain.cpp
        TARGETS eacp-svg)
