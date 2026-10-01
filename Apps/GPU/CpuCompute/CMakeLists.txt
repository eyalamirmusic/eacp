add_executable(CpuCompute Main.cpp)
target_link_libraries(CpuCompute PRIVATE eacp-gpu eacp-cpu-compute)

set(BUNDLE_ID "com.eacp.cpucompute")

set_target_properties(CpuCompute PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "CpuCompute"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(CpuCompute)
