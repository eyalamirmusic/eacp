nano_add_executable(SA3DiTUnitTests
        SOURCES
            UnitTests.cpp
        TARGETS eacp-sa3-dit)

set(sa3_dit_checkpoint_path
        "$ENV{HOME}/.cache/huggingface/hub/models--stabilityai--stable-audio-3-small-music/snapshots/0fef1392cd842149a2b6d445e181c97608faac06/model.safetensors")

set(sa3_dit_medium_checkpoint_path
        "$ENV{HOME}/.cache/huggingface/hub/models--stabilityai--stable-audio-3-medium/snapshots/27b5a21b791b1b033d193a9e1e3ce78493f102f9/model.safetensors")

function(sa3_dit_add_golden_test name source)
    nano_add_executable(${name}
            SOURCES
                ${source}
            TARGETS eacp-sa3-dit)

    target_compile_definitions(${name} PRIVATE
            SA3_DIT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData"
            SA3_DIT_CHECKPOINT_PATH="${sa3_dit_checkpoint_path}"
            SA3_DIT_MEDIUM_CHECKPOINT_PATH="${sa3_dit_medium_checkpoint_path}")
endfunction()

sa3_dit_add_golden_test(SA3DiTTimestepEmbeddingGoldenTests TimestepEmbeddingGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTGlobalConditioningGoldenTests GlobalConditioningGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTSingleLayerGoldenTests SingleLayerGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTForwardGoldenTests ForwardGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumTimestepEmbeddingGoldenTests MediumTimestepEmbeddingGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumGlobalConditioningGoldenTests MediumGlobalConditioningGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumSingleLayerGoldenTests MediumSingleLayerGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumForwardGoldenTests MediumForwardGoldenTests.cpp)
