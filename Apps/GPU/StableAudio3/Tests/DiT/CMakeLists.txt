nano_add_executable(SA3DiTUnitTests
        SOURCES
            UnitTests.cpp
        TARGETS eacp-sa3-dit)

function(sa3_dit_add_golden_test name source)
    nano_add_executable(${name}
            SOURCES
                ${source}
            TARGETS eacp-sa3-dit eacp-sa3-checkpoints)

    target_compile_definitions(${name} PRIVATE
            SA3_DIT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")
endfunction()

sa3_dit_add_golden_test(SA3DiTTimestepEmbeddingGoldenTests TimestepEmbeddingGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTGlobalConditioningGoldenTests GlobalConditioningGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTSingleLayerGoldenTests SingleLayerGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTForwardGoldenTests ForwardGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumTimestepEmbeddingGoldenTests MediumTimestepEmbeddingGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumGlobalConditioningGoldenTests MediumGlobalConditioningGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumSingleLayerGoldenTests MediumSingleLayerGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumForwardGoldenTests MediumForwardGoldenTests.cpp)
sa3_dit_add_golden_test(SA3DiTMediumVariesWithInputTests MediumVariesWithInputTests.cpp)
