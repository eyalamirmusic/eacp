target_sources(eacp PRIVATE
        Threads/TaskSemaphore.cpp
        Threads/EventLoop.cpp
        App/App.cpp
)

if (APPLE)
    target_sources(eacp PRIVATE
            App/App.mm
            Network/Http.mm
            ObjC/AutoReleasePool.mm

            Threads/EventLoop.mm
            Threads/Timer.mm
            Threads/ThreadUtils.mm

            Graphics/Window.mm
            Graphics/View.mm)
endif ()