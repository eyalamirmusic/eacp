nano_add_executable(SVGImageTests
        SOURCES SVGImageTests.cpp
        TARGETS eacp-svg)

target_compile_definitions(SVGImageTests PRIVATE
        EACP_SVG_TEST_FILES="${CMAKE_CURRENT_SOURCE_DIR}/TestFiles")
