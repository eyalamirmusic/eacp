nano_add_executable(SA3TextEncoderTokenizerFastTests
        SOURCES
            TokenizerFastTests.cpp
        TARGETS eacp-sa3-text-encoder)

nano_add_executable(SA3TextEncoderTokenizerGoldenTests
        SOURCES
            TokenizerGoldenTests.cpp
        TARGETS eacp-sa3-text-encoder)

target_compile_definitions(SA3TextEncoderTokenizerGoldenTests PRIVATE
        SA3_TEXT_ENCODER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")

nano_add_executable(SA3TextEncoderLayer0Tests
        SOURCES
            Layer0Tests.cpp
        TARGETS eacp-sa3-text-encoder)

target_compile_definitions(SA3TextEncoderLayer0Tests PRIVATE
        SA3_TEXT_ENCODER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")

nano_add_executable(SA3TextEncoderFullTests
        SOURCES
            FullEncoderTests.cpp
        TARGETS eacp-sa3-text-encoder)

target_compile_definitions(SA3TextEncoderFullTests PRIVATE
        SA3_TEXT_ENCODER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")
