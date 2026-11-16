#include "GlobalMouseClickMonitor.h"

#import <Cocoa/Cocoa.h>

namespace eacp::Graphics
{

struct GlobalMouseClickMonitor::Native
{
    explicit Native(Callback callback)
        : onPressed(std::move(callback))
    {
        auto pressMask = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
                         | NSEventMaskOtherMouseDown;
        monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:pressMask
                                                         handler:^(NSEvent*) {
                                                           onPressed();
                                                         }];
    }

    ~Native()
    {
        if (monitor != nil)
            [NSEvent removeMonitor:monitor];
    }

    Callback onPressed;
    id monitor = nil;
};

GlobalMouseClickMonitor::GlobalMouseClickMonitor(Callback onPressed)
    : impl(std::move(onPressed))
{
}

GlobalMouseClickMonitor::~GlobalMouseClickMonitor() = default;

} // namespace eacp::Graphics
