add_executable(ComputeParticles Main.cpp)
target_link_libraries(ComputeParticles PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.computeparticles")

set_target_properties(ComputeParticles PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ComputeParticles"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(ComputeParticles)
set_default_target_setting(ComputeParticles)
