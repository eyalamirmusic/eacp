find_package(NanoTest REQUIRED)

if (APPLE)
    add_subdirectory(ObjC)
endif ()

add_subdirectory(Network)
