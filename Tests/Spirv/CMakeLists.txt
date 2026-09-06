# eacp-spirv is absent when EACP_BUILD_SPIRV is off - its default everywhere
# but Linux - and on a tree that never entered Lib/eacp/GPU at all, so the
# guard is the target rather than the option.
if (NOT TARGET eacp-spirv)
    return ()
endif ()

nano_add_executable(SpirvTests
        SOURCES SpirvTests.cpp
        TARGETS eacp-spirv)
