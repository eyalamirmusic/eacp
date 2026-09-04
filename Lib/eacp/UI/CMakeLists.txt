add_library(eacp-ui STATIC)

# A lightweight component tier hosted in a single GPUView, drawn through its own
# distance-field shape batcher and eacp-text's glyph batcher. Portable C++ over
# the GPU module's public API - no APPLE/WIN32 branches - because everything
# per-platform is already below it in the GPU and text modules.
target_link_libraries(eacp-ui PUBLIC eacp-gpu eacp-gpuwidgets eacp-text)

target_sources(eacp-ui PRIVATE
        Component/Component.cpp
        DragAndDrop/DragAndDrop.cpp
        Graphics/Graphics.cpp
        Host/ComponentHost.cpp
        Render/CoverageAtlas.cpp
        Render/DrawList.cpp
        Render/DrawPlayer.cpp
        Render/GradientRamps.cpp
        Render/ImageBatch.cpp
        Render/ImageCache.cpp
        Render/Layer.cpp
        Render/LayerRenderer.cpp
        Render/MaskCache.cpp
        Render/MeshBatch.cpp
        Render/PathShape.cpp
        Render/ShapeBatch.cpp
        Widgets/ComboBox.cpp
        Widgets/ListBox.cpp
        Widgets/Tabs.cpp
        Widgets/Widgets.cpp)

set_default_target_setting(eacp-ui)
eacp_enable_unity_build(eacp-ui)
add_ide_sources(eacp-ui)
