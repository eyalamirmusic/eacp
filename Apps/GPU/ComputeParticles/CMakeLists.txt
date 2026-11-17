eacp_add_app(ComputeParticles Main.cpp
        DESCRIPTION "40,000 particles integrated by a kernel and drawn in the same frame.")
target_link_libraries(ComputeParticles PRIVATE eacp-gpu)
set_target_properties(ComputeParticles PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(ComputeParticles)
set_default_target_setting(ComputeParticles)
