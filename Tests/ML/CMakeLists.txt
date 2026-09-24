nano_add_executable(MLGraphTests
        SOURCES
            GraphApplyTests.cpp
            GraphBlobTests.cpp
            GraphCoreMLTests.cpp
            GraphEncoderTests.cpp
            GraphPackageTests.cpp
            GraphProtobufTests.cpp
            GraphShapeTests.cpp
            GraphTextTests.cpp
        TARGETS eacp-ml-graph)

if (TARGET eacp-ml)
    target_link_libraries(MLGraphTests PRIVATE eacp-ml)
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
            PlacementTests.cpp
            ProgramTests.cpp
            TestMain.cpp
        TARGETS eacp-ml)
