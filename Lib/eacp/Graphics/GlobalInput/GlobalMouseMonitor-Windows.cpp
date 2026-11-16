#include "GlobalMouseMonitor-Windows.h"
#include "GlobalMouseMonitorDelivery.h"

#include "../Graphics/Keyboard.h"
#include "../Window/Display.h"

namespace eacp::Graphics
{
namespace
{
struct MessageForMouseType
{
    WPARAM message;
    MouseEventType type;
    MouseButton button;
};

constexpr MessageForMouseType messageTypes[] = {
    {WM_LBUTTONDOWN, MouseEventType::Down, MouseButton::Left},
    {WM_RBUTTONDOWN, MouseEventType::Down, MouseButton::Right},
    {WM_MBUTTONDOWN, MouseEventType::Down, MouseButton::Middle},
    {WM_XBUTTONDOWN, MouseEventType::Down, MouseButton::Other},
    {WM_LBUTTONUP, MouseEventType::Up, MouseButton::Left},
    {WM_RBUTTONUP, MouseEventType::Up, MouseButton::Right},
    {WM_MBUTTONUP, MouseEventType::Up, MouseButton::Middle},
    {WM_XBUTTONUP, MouseEventType::Up, MouseButton::Other},
    {WM_MOUSEMOVE, MouseEventType::Moved, MouseButton::Left},
    {WM_MOUSEWHEEL, MouseEventType::Wheel, MouseButton::Other},
    {WM_MOUSEHWHEEL, MouseEventType::Wheel, MouseButton::Other},
};

std::optional<MessageForMouseType> findMessageType(WPARAM message)
{
    for (const auto& entry: messageTypes)
    {
        if (entry.message == message)
            return entry;
    }

    return std::nullopt;
}

// In lines, as a window's own wheel messages report it: a detent is
// WHEEL_DELTA, and a precision touchpad sends fractions of one.
Point getWheelDelta(const LowLevelMouseInput& input)
{
    auto wheelDelta = static_cast<short>(HIWORD(input.data.mouseData));
    auto lines = static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA);

    if (input.message == WM_MOUSEHWHEEL)
        return {lines, 0.f};

    return {0.f, lines};
}

constexpr auto millisecondsPerSecond = 1000.0;
} // namespace

void LowLevelMouseTranslator::trackButtons(const MouseEvent& event)
{
    if (event.type == MouseEventType::Down)
    {
        ++buttonsHeld;
        lastDownPosition = event.pos;
        lastDownButton = event.button;
    }

    if (event.type == MouseEventType::Up)
        buttonsHeld = std::max(0, buttonsHeld - 1);
}

std::optional<MouseEvent>
    LowLevelMouseTranslator::translate(const LowLevelMouseInput& input,
                                       float pixelsPerPoint)
{
    auto entry = findMessageType(input.message);

    if (!entry)
        return std::nullopt;

    auto event = MouseEvent {};
    event.type = entry->type;
    event.button = entry->button;
    event.pos = {static_cast<float>(input.data.pt.x) / pixelsPerPoint,
                 static_cast<float>(input.data.pt.y) / pixelsPerPoint};
    event.timestamp = static_cast<double>(input.data.time) / millisecondsPerSecond;

    if (event.type == MouseEventType::Moved && buttonsHeld > 0)
    {
        event.type = MouseEventType::Dragged;
        event.button = lastDownButton;
    }

    trackButtons(event);

    if (event.type == MouseEventType::Wheel)
    {
        event.delta = getWheelDelta(input);
        event.downPos = event.pos;
    }
    else
    {
        event.delta = {event.pos.x - lastPosition.x, event.pos.y - lastPosition.y};
        event.downPos = lastDownPosition;
        lastPosition = event.pos;
    }

    event.rawDelta = event.delta;
    return event;
}

namespace
{
struct LiveMonitor
{
    std::function<void(const LowLevelMouseInput&)> report;
};

// A low-level hook has no user data, so the live monitors share one hook and
// are listed here. Only the main thread touches these: the hook runs on the
// thread that installed it.
EA::Vector<LiveMonitor*>& getLiveMonitors()
{
    static auto monitors = EA::Vector<LiveMonitor*> {};
    return monitors;
}

HHOOK& getSharedHook()
{
    static auto hook = HHOOK {nullptr};
    return hook;
}

LRESULT CALLBACK onLowLevelMouse(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION)
    {
        auto input =
            LowLevelMouseInput {message, *reinterpret_cast<MSLLHOOKSTRUCT*>(data)};

        for (auto* monitor: getLiveMonitors())
            monitor->report(input);
    }

    return CallNextHookEx(nullptr, code, message, data);
}

void addLiveMonitor(LiveMonitor& monitor)
{
    auto& hook = getSharedHook();

    if (hook == nullptr)
    {
        hook = SetWindowsHookExW(
            WH_MOUSE_LL, onLowLevelMouse, GetModuleHandleW(nullptr), 0);
    }

    getLiveMonitors().add(&monitor);
}

void removeLiveMonitor(LiveMonitor& monitor)
{
    auto& monitors = getLiveMonitors();
    monitors.removeAllMatches(&monitor);

    auto& hook = getSharedHook();

    if (monitors.empty() && hook != nullptr)
    {
        UnhookWindowsHookEx(hook);
        hook = nullptr;
    }
}
} // namespace

struct GlobalMouseMonitor::Native
{
    Native(EA::Vector<MouseEventType> typesToReportToUse, Handler onEvent)
        : typesToReport(std::move(typesToReportToUse))
        , handler(std::make_shared<Handler>(std::move(onEvent)))
        , pixelsPerPoint(primaryDisplay().backingScale)
    {
        if (typesToReport.empty())
            return;

        liveMonitor.report = [this](const LowLevelMouseInput& input)
        { report(input); };
        addLiveMonitor(liveMonitor);
    }

    ~Native() { removeLiveMonitor(liveMonitor); }

    // Translated even when not asked for, so drags and downPos stay right.
    void report(const LowLevelMouseInput& input)
    {
        auto event = translator.translate(input, pixelsPerPoint);

        if (!event || !typesToReport.contains(event->type))
            return;

        event->modifiers = Keyboard::getModifiers();
        deliverOnMainThread(handler, *event);
    }

    EA::Vector<MouseEventType> typesToReport;
    std::shared_ptr<Handler> handler;
    float pixelsPerPoint = 1.f;
    LowLevelMouseTranslator translator;
    LiveMonitor liveMonitor;
};

GlobalMouseMonitor::GlobalMouseMonitor(EA::Vector<MouseEventType> typesToReport,
                                       Handler onEvent)
    : impl(std::move(typesToReport), std::move(onEvent))
{
}

GlobalMouseMonitor::~GlobalMouseMonitor() = default;

} // namespace eacp::Graphics
