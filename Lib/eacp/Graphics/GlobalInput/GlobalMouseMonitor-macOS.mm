#include "GlobalMouseMonitor-macOS.h"
#include "GlobalMouseMonitorDelivery.h"

#include "../View/View-MacOS.h"
#include "../Window/Window-macOS.h"

namespace eacp::Graphics
{
namespace
{
struct NativeTypeForMouseType
{
    NSEventType nativeType;
    MouseEventType type;
};

constexpr NativeTypeForMouseType nativeTypes[] = {
    {NSEventTypeLeftMouseDown, MouseEventType::Down},
    {NSEventTypeRightMouseDown, MouseEventType::Down},
    {NSEventTypeOtherMouseDown, MouseEventType::Down},
    {NSEventTypeLeftMouseUp, MouseEventType::Up},
    {NSEventTypeRightMouseUp, MouseEventType::Up},
    {NSEventTypeOtherMouseUp, MouseEventType::Up},
    {NSEventTypeLeftMouseDragged, MouseEventType::Dragged},
    {NSEventTypeRightMouseDragged, MouseEventType::Dragged},
    {NSEventTypeOtherMouseDragged, MouseEventType::Dragged},
    {NSEventTypeMouseMoved, MouseEventType::Moved},
    {NSEventTypeScrollWheel, MouseEventType::Wheel},
    {NSEventTypeMagnify, MouseEventType::Magnify},
};

std::optional<MouseEventType> findMouseEventType(NSEventType nativeType)
{
    for (const auto& entry: nativeTypes)
    {
        if (entry.nativeType == nativeType)
            return entry.type;
    }

    return std::nullopt;
}

bool isPressNeeded(const EA::Vector<MouseEventType>& types)
{
    return types.contains(MouseEventType::Up) || types.contains(MouseEventType::Dragged);
}

// A global monitor's event has no window and its location is already on the
// screen; a local monitor's is in its window.
Point getDesktopPosition(NSEvent* event)
{
    auto screenPoint = event.locationInWindow;

    if (event.window != nil)
        screenPoint = [event.window convertPointToScreen:screenPoint];

    return {(float) screenPoint.x, (float) (primaryScreenTop() - screenPoint.y)};
}

bool isGesture(MouseEventType type)
{
    return type == MouseEventType::Wheel || type == MouseEventType::Magnify;
}
} // namespace

NSEventMask getEventMaskForTypes(const EA::Vector<MouseEventType>& types)
{
    auto mask = NSEventMask {0};

    for (const auto& entry: nativeTypes)
    {
        auto isAskedFor = types.contains(entry.type)
                          || (entry.type == MouseEventType::Down && isPressNeeded(types));

        if (isAskedFor)
            mask |= NSEventMaskFromType(entry.nativeType);
    }

    return mask;
}

std::optional<MouseEvent> GlobalMouseEventTranslator::translate(NSEvent* event)
{
    auto type = findMouseEventType(event.type);

    if (!type)
        return std::nullopt;

    auto mouseEvent = mouseEventFromNSEvent(event, *type);
    mouseEvent.pos = getDesktopPosition(event);

    if (mouseEvent.type == MouseEventType::Down)
        lastDownPosition = mouseEvent.pos;

    mouseEvent.downPos = isGesture(mouseEvent.type) ? mouseEvent.pos : lastDownPosition;
    return mouseEvent;
}

struct GlobalMouseMonitor::Native
{
    Native(EA::Vector<MouseEventType> typesToReportToUse, Handler onEvent)
        : typesToReport(std::move(typesToReportToUse))
        , handler(std::make_shared<Handler>(std::move(onEvent)))
    {
        auto mask = getEventMaskForTypes(typesToReport);

        if (mask == 0)
            return;

        globalMonitor =
            [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                   handler:^(NSEvent* event) {
                                                     report(event);
                                                   }];

        localMonitor =
            [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                  handler:^NSEvent*(NSEvent* event) {
                                                    report(event);
                                                    return event;
                                                  }];
    }

    ~Native()
    {
        if (globalMonitor != nil)
            [NSEvent removeMonitor:globalMonitor];

        if (localMonitor != nil)
            [NSEvent removeMonitor:localMonitor];
    }

    void report(NSEvent* event)
    {
        auto mouseEvent = translator.translate(event);

        if (mouseEvent && typesToReport.contains(mouseEvent->type))
            deliverOnMainThread(handler, *mouseEvent);
    }

    EA::Vector<MouseEventType> typesToReport;
    std::shared_ptr<Handler> handler;
    GlobalMouseEventTranslator translator;
    id globalMonitor = nil;
    id localMonitor = nil;
};

GlobalMouseMonitor::GlobalMouseMonitor(EA::Vector<MouseEventType> typesToReport,
                                       Handler onEvent)
    : impl(std::move(typesToReport), std::move(onEvent))
{
}

GlobalMouseMonitor::~GlobalMouseMonitor() = default;

} // namespace eacp::Graphics
