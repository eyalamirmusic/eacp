# Header-only helpers that tie eacp's async primitives into
# NanoTest. Lives in its own target so eacp-core itself stays free
# of the NanoTest dependency.
find_package(NanoTest REQUIRED)

add_library(eacp-core-testing INTERFACE)
target_link_libraries(eacp-core-testing INTERFACE eacp-core NanoTest)
add_ide_sources(eacp-core-testing AsyncTest.h)
