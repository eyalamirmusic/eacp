nano_add_executable(MLGraphTests
        SOURCES
            GraphApplyTests.cpp
            GraphBlobTests.cpp
            GraphCoreMLTests.cpp
            GraphDecoderStepTests.cpp
            GraphEncoderTests.cpp
            GraphPackageTests.cpp
            GraphProtobufTests.cpp
            GraphShapeTests.cpp
            GraphTextTests.cpp
        TARGETS eacp-ml-graph)

if (TARGET eacp-ml)
    target_link_libraries(MLGraphTests PRIVATE eacp-ml)
endif ()

if (TARGET eacp-ml-kernels)
    nano_add_executable(MLKernelTests
            SOURCES
                ActivationTests.cpp
                AttentionTests.cpp
                BandedAttentionTests.cpp
                LinearTests.cpp
                NormTests.cpp
                RoPETests.cpp
                SafetensorsFileTests.cpp
                SwiGLUTests.cpp
                TensorOpsTests.cpp
                TensorTests.cpp
            TARGETS eacp-ml-kernels)
endif ()

if (NOT EACP_HAS_COREML)
    return ()
endif ()

nano_add_executable(MLTests
        NO_MAIN
        SOURCES
            MultiArrayTests.cpp
            AsyncTests.cpp
            CacheTests.cpp
            DecoderStepReference.cpp
            DecoderStepTests.cpp
            EncoderReference.cpp
            EncoderTests.cpp
            PlacementTests.cpp
            ProgramTests.cpp
            TestMain.cpp
        TARGETS eacp-ml)

set_source_files_properties(EncoderReference.cpp DecoderStepReference.cpp
        PROPERTIES COMPILE_OPTIONS -O2 SKIP_PRECOMPILE_HEADERS ON)
