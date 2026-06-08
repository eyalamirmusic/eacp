add_library(eacp-gpuwidgets STATIC)

# GPUWidgets builds on the GPU module's platform-agnostic public API only (shader
# EDSL, buffers, render pass, GPUView), so every source is portable C++ - no
# APPLE/WIN32 branches. eacp-gpu links eacp-graphics publicly, so the Graphics
# primitives (Point, Rect, Color) come through transitively.
target_link_libraries(eacp-gpuwidgets PUBLIC eacp-gpu)

target_sources(eacp-gpuwidgets PRIVATE
        Gradient.cpp
        Path/Path.cpp
        Path/PathTessellator.cpp
        View/PathView.cpp)

set_default_target_setting(eacp-gpuwidgets)
eacp_enable_unity_build(eacp-gpuwidgets)
add_ide_sources(eacp-gpuwidgets)
