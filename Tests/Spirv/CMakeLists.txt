# The target rather than the option: a tree that skipped Lib/eacp/GPU never
# saw the option either.
if (NOT TARGET eacp-spirv)
    return ()
endif ()

nano_add_executable(SpirvTests
        SOURCES SpirvTests.cpp
        TARGETS eacp-spirv)
