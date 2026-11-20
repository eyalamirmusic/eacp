#include <eacp/Core/Utils/Format.h>
#include <eacp/Graphics/GlobalInput/GlobalMouseMonitor.h>
#include <eacp/UI/UI.h>

#include <optional>

// Graphics::GlobalMouseMonitor: every mouse event of the ticked kinds, from any
// app's windows and this one's, listed as it arrives with its desktop position.
// Untick a kind and the monitor is rebuilt without it, so the list stops
// growing on moves the moment Moved is off. On Linux the monitor never fires,
// and the window says so.

using namespace eacp;

namespace
{
constexpr auto maximumLines = 500;
constexpr auto rowHeight = 18.f;
constexpr auto toggleHeight = 26.f;
constexpr auto toggleWidth = 110.f;
constexpr auto margin = 12.f;

struct KindToggle
{
    Graphics::MouseEventType type;
    const char* name;
    bool startsOn;
};

constexpr KindToggle kindToggles[] = {
    {Graphics::MouseEventType::Down, "Down", true},
    {Graphics::MouseEventType::Up, "Up", true},
    {Graphics::MouseEventType::Dragged, "Dragged", false},
    {Graphics::MouseEventType::Moved, "Moved", false},
    {Graphics::MouseEventType::Wheel, "Wheel", true},
    {Graphics::MouseEventType::Magnify, "Magnify", false},
};

struct ButtonName
{
    Graphics::MouseButton button;
    const char* name;
};

constexpr ButtonName buttonNames[] = {
    {Graphics::MouseButton::Left, "left"},
    {Graphics::MouseButton::Right, "right"},
    {Graphics::MouseButton::Middle, "middle"},
    {Graphics::MouseButton::Other, "other"},
};

std::string getTypeName(Graphics::MouseEventType type)
{
    for (const auto& kind: kindToggles)
    {
        if (kind.type == type)
            return kind.name;
    }

    return "?";
}

std::string getButtonName(Graphics::MouseButton button)
{
    for (const auto& entry: buttonNames)
    {
        if (entry.button == button)
            return entry.name;
    }

    return "?";
}

std::string describeEvent(const Graphics::MouseEvent& event)
{
    return Strings::format("{} {} at ({}, {}), pressed at ({}, {}), delta ({}, {})",
                           getTypeName(event.type),
                           getButtonName(event.button),
                           event.pos.x,
                           event.pos.y,
                           event.downPos.x,
                           event.downPos.y,
                           event.delta.x,
                           event.delta.y);
}

std::string getPlatformNote()
{
    if constexpr (Platform::isMac() || Platform::isWindows())
        return "Ticked kinds are reported from every app's windows, this one's included.";

    return "This platform has no global mouse monitor: nothing will arrive here.";
}

struct EventList final
    : UI::Component
    , UI::ListBoxModel
{
    EventList()
    {
        list.setModel(this);
        list.setRowHeight(rowHeight);
        addChildren({list});
    }

    void add(const std::string& line)
    {
        if (lines.size() >= maximumLines)
            lines.erase(lines.begin());

        lines.add(line);
        list.updateContent();
        list.scrollToRow(lines.size() - 1);
    }

    int getNumRows() override { return lines.size(); }

    void paintRow(UI::Graphics& g, int row, const UI::Rect& rowBounds, bool) override
    {
        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::text));
        g.drawText(lines[row], rowBounds.inset(8.f, 0.f));
    }

    void resized() override { list.setBounds(getLocalBounds()); }

    UI::ListBox list;
    Vector<std::string> lines;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        note.setThemeColour(UI::ThemeKeys::dimText);
        addChildren({note, events});

        for (const auto& kind: kindToggles)
        {
            auto& toggle = toggles.createNew(std::string {kind.name});
            toggle.setChecked(kind.startsOn);
            toggle.onChange = [this](bool) { rebuildMonitor(); };
            addChildren({toggle});
        }

        rebuildMonitor();
    }

    EA::Vector<Graphics::MouseEventType> getTickedTypes()
    {
        auto types = EA::Vector<Graphics::MouseEventType> {};

        for (auto index = 0; index < toggles.size(); ++index)
        {
            if (toggles[index]->isChecked())
                types.add(kindToggles[index].type);
        }

        return types;
    }

    void rebuildMonitor()
    {
        monitor.reset();
        monitor.emplace(getTickedTypes(),
                        [this](const Graphics::MouseEvent& event)
                        { events.add(describeEvent(event)); });
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().get(UI::ThemeKeys::background));
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(margin);
        auto row = area.removeFromTop(toggleHeight);

        for (auto& toggle: toggles)
            toggle->setBounds(row.removeFromLeft(toggleWidth));

        area.removeFromTop(margin / 2.f);
        note.setBounds(area.removeFromTop(rowHeight));
        area.removeFromTop(margin / 2.f);
        events.setBounds(area);
    }

    UI::Label note {getPlatformNote()};
    EA::OwnedVector<UI::Checkbox> toggles;
    EventList events;
    std::optional<Graphics::GlobalMouseMonitor> monitor;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        setRootComponent(root);
    }

    DemoRoot root;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 760;
    options.height = 520;
    options.title = "eacp — GlobalMouseMonitor";
    return options;
}
} // namespace

int main()
{
    return Graphics::runWindowedApp<DemoHost>(makeOptions());
}
