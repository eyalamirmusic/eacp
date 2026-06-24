add_executable(Lattice Main.cpp)
target_link_libraries(Lattice PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.lattice")

set_target_properties(Lattice PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Lattice"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(Lattice)

# Drivable + recordable by an agent over MCP with no app-code changes:
# the harness launcher, ASan, the window debug/capture server (screenshot
# + recording + input injection), and a stable signature for TCC.
eacp_enable_dev_affordances(Lattice)
