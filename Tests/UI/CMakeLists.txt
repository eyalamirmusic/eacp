set(ui_test_targets eacp-ui eacp-sprites)

if (EACP_HAS_NETWORK)
    list(APPEND ui_test_targets eacp-ui-network)
endif ()

nano_add_executable(UITests
        SOURCES
            ClipMaskTests.cpp
            ClippedFillTests.cpp
            ComboBoxTests.cpp
            CoverageAtlasTests.cpp
            DrawListTests.cpp
            DragAndDropTests.cpp
            GradientRampsTests.cpp
            ImageBatchTests.cpp
            KeyboardTests.cpp
            ListBoxTests.cpp
            MaskCacheTests.cpp
            LayerTests.cpp
            ModuleShaderTests.cpp
            RepaintTests.cpp
            ShadowTests.cpp
            SliderTests.cpp
            TabsTests.cpp
            TextEditorTests.cpp
        TARGETS ${ui_test_targets})

if (EACP_HAS_NETWORK)
    target_sources(UITests PRIVATE OnlineResourceMonitorTests.cpp)
endif ()

# ModuleShaderTests emits every shader this tier builds as GLSL and compiles it
# with glslang wherever eacp-spirv is built. The helper comes from Tests/GPU.
eacp_enable_glsl_compile_checks(UITests)
