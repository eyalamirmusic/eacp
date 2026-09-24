nano_add_executable(MLTests
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
        TARGETS eacp-ml)
