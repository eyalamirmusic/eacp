target_sources(eacp PRIVATE Threads/TaskSemaphore.cpp)

if (APPLE)
    target_sources(eacp PRIVATE
            Network/Http.mm
            ObjC/AutoReleasePool.mm)
endif ()