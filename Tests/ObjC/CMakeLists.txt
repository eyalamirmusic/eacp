nano_add_executable(ObjCPtrTests
        SOURCES ObjCPtrTests.mm
        TARGETS eacp-core)

set_target_properties(ObjCPtrTests PROPERTIES
        OBJCXX_STANDARD 20)

target_compile_options(ObjCPtrTests PRIVATE
        -fno-objc-arc)

target_link_libraries(ObjCPtrTests PRIVATE
        "-framework Foundation")
