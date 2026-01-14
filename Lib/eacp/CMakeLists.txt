target_sources(eacp PRIVATE
        Threads/TaskSemaphore.cpp
        Threads/EventLoop.cpp
        App/App.cpp
        Graphics/Primitives.cpp
)

if (APPLE)
    target_sources(eacp PRIVATE
            App/App.mm
            Network/Http.mm
            ObjC/AutoReleasePool.mm

            Threads/EventLoop.mm
            Threads/Timer.mm
            Threads/DisplayLink.mm
            Threads/ThreadUtils.mm

            Graphics/Window.mm
            Graphics/View.mm

            Graphics/Path.mm
            Graphics/Font.mm
            Graphics/MacGraphicsContext.mm
            Graphics/MacGraphicUtils.mm
            Graphics/ShapeLayer.mm
            Graphics/TextLayer.mm)
endif ()