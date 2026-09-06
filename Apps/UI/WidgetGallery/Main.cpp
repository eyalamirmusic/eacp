#include <eacp/UI/UI.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// The stock widgets, in one window, doing the things that are hard to see from
// a header.
//
// The window is a TabbedComponent filling it, so the tabs are not a page of the
// demo but the demo's own frame: the page that is not showing is hidden, and a
// hidden subtree is stepped over by the recording walk, so the list of ten
// thousand rows on the second page costs nothing at all while the first is up.
//
// "Controls" is about gestures. Every callback of every control on it writes a
// line into the log beside them, so what a host recording automation would see
// is on screen: a knob drag is exactly one dragStart, a run of values and
// exactly one dragEnd, and a double-click reset -- every control here has a
// default value to reset to -- is all three at once rather than a bare value
// arriving with no gesture around it. The second combo box is hard against the
// bottom edge on purpose. A component tree may be a plugin editor inside
// somebody else's window, so its list has nowhere to spill: it opens upward,
// and being longer than the page is tall, it scrolls.
//
// "List" is about the other kind of cost. The left-hand list is ten thousand
// rows and one component, because a row is something a model paints rather
// than something the tree holds; the right-hand one is the same widget with
// columns, which is what makes it a table. The footer reports the same figures
// ComponentTree does, and the one to read is the component count -- it is a
// three-digit number with ten thousand rows on screen.

using namespace eacp;

namespace
{
constexpr auto padding = 12.f;
constexpr auto generatedRowCount = 10000;

// Which page the window opens on, from the command line, so a picture of the
// second one costs no click.
int startupTab = 0;

std::string percentText(float value)
{
    return std::to_string(std::lround(value * 100.f)) + "%";
}

std::string hertzText(float hertz)
{
    auto buffer = std::array<char, 32> {};
    std::snprintf(buffer.data(), buffer.size(), "%.2f Hz", (double) hertz);

    return buffer.data();
}

std::string midiNoteName(int note)
{
    constexpr const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    return std::string {names[note % 12]} + std::to_string(note / 12 - 1);
}

float midiNoteHertz(int note)
{
    return 440.f * std::pow(2.f, (float) (note - 69) / 12.f);
}

std::string generatedRowText(int row)
{
    return "Row " + std::to_string(row + 1) + "   voice "
           + std::to_string(row % 64 + 1) + "   " + midiNoteName(row % 128);
}

UI::Rect centredStrip(const UI::Rect& area, float height)
{
    return {area.x, area.y + (area.h - height) * 0.5f, area.w, height};
}

UI::Rect centredColumn(const UI::Rect& area, float width)
{
    return {area.x + (area.w - width) * 0.5f, area.y, width, area.h};
}

// Selection and banding, which every model here draws the same way so the two
// lists look like one widget rather than two.
void paintRowBackground(UI::Graphics& g,
                        int row,
                        const UI::Rect& bounds,
                        bool selected)
{
    const auto& theme = UI::defaultTheme();

    if (selected)
    {
        g.setColour(theme.accentDim);
        g.fillRect(bounds);
    }
    else if (row % 2 == 1)
    {
        g.setColour(theme.panel);
        g.fillRect(bounds);
    }
}

// The log every control on the first page writes to: a ListBox over a vector of
// strings, newest last and scrolled to.
struct EventLog final
    : UI::Component
    , UI::ListBoxModel
{
    EventLog()
    {
        title.setColour(UI::defaultTheme().dimText);

        list.setModel(this);
        list.setRowHeight(18.f);

        addChildren({title, list});
    }

    void add(const std::string& widget, const std::string& event)
    {
        lines.add(std::to_string(lines.size() + 1) + "   " + widget + "   " + event);

        list.updateContent();
        list.scrollToRow(lines.size() - 1);
    }

    int getNumRows() override { return lines.size(); }

    void paintRow(UI::Graphics& g,
                  int row,
                  const UI::Rect& bounds,
                  bool selected) override
    {
        paintRowBackground(g, row, bounds, selected);

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(lines[row], bounds.inset(8.f, 0.f));
    }

    // The list fills itself with the background colour, so an empty one is
    // invisible without a frame drawn over it.
    void paintOverChildren(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().outline);
        g.drawRect(list.getBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds();

        title.setBounds(area.removeFromTop(20.f));
        list.setBounds(area);
    }

    UI::Label title {"Event log — every callback, in order"};
    UI::ListBox list;
    Vector<std::string> lines;
};

// A control with its name over it and its value under it, which is what a knob
// or a fader on a panel actually is. Knob and Slider answer the same calls, so
// this is written once and instantiated for both.
template <typename Control>
struct LabelledControl final : UI::Component
{
    template <typename... Args>
    LabelledControl(EventLog& logToUse,
                    std::string nameToUse,
                    float startValue,
                    Args&&... args)
        : log(logToUse)
        , name(std::move(nameToUse))
        , control(std::forward<Args>(args)...)
        , caption(name)
    {
        caption.setColour(UI::defaultTheme().dimText);
        caption.setJustification(UI::Justification::Centred);
        readout.setJustification(UI::Justification::Centred);

        control.setDefaultValue(startValue);
        control.setValue(startValue);
        showValue();

        control.onDragStart = [this] { log.add(name, "dragStart"); };
        control.onDragEnd = [this] { log.add(name, "dragEnd"); };

        control.onValueChange = [this](float value)
        {
            showValue();
            log.add(name, "value " + percentText(value));
        };

        addChildren({caption, control, readout});
    }

    void showValue() { readout.setText(percentText(control.getValue())); }

    void resized() override
    {
        auto area = getLocalBounds();

        caption.setBounds(area.removeFromTop(16.f));
        readout.setBounds(area.removeFromBottom(16.f));
        control.setBounds(shapeControl(area.inset(6.f, 4.f)));
    }

    // What the control takes out of the middle: a knob fills it, a fader is a
    // strip or a column across it, the track being the whole of its bounds.
    std::function<UI::Rect(const UI::Rect&)> shapeControl = [](const UI::Rect& area)
    { return area; };

    EventLog& log;
    std::string name;
    Control control;
    UI::Label caption;
    UI::Label readout;
};

struct ControlsPage final : UI::Component
{
    ControlsPage()
    {
        presetCaption.setColour(UI::defaultTheme().dimText);
        editorCaption.setColour(UI::defaultTheme().dimText);
        programCaption.setColour(UI::defaultTheme().dimText);

        preset.addItems({"Init", "Warm", "Bright", "Hollow", "Bell"});
        preset.setSelectedIndex(0);
        preset.onChange = [this](int index)
        { log.add("preset", "change " + preset.getItemText(index)); };

        for (auto index = 0; index < 40; ++index)
            program.addItem("Program " + std::to_string(index + 1));

        program.setSelectedIndex(0);
        program.onChange = [this](int index)
        { log.add("program", "change " + program.getItemText(index)); };

        trigger.onClick = [this] { log.add("trigger", "click"); };

        latch.setToggleable(true);
        latch.onClick = [this]
        { log.add("latch", latch.getToggleState() ? "click on" : "click off"); };

        bypass.onChange = [this](bool on)
        { log.add("bypass", on ? "change on" : "change off"); };

        link.onChange = [this](bool on)
        { log.add("link", on ? "change on" : "change off"); };

        editor.setPlaceholder("patch name");
        editor.onReturnKey = [this](const std::string& text)
        { log.add("name", "return \"" + text + "\""); };

        level.shapeControl = [](const UI::Rect& area)
        { return centredStrip(area, 22.f); };

        leftFader.shapeControl = [](const UI::Rect& area)
        { return centredColumn(area, 24.f); };

        rightFader.shapeControl = [](const UI::Rect& area)
        { return centredColumn(area, 24.f); };

        addChildren({log, presetCaption, preset, gain, tone, mix});
        addChildren({level, leftFader, rightFader, trigger, latch, bypass, link});
        addChildren({editorCaption, editor, programCaption, program});
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        log.setBounds(area.removeFromRight(340.f));
        area.removeFromRight(padding);

        auto top = area.removeFromTop(26.f);
        presetCaption.setBounds(top.removeFromLeft(70.f));
        preset.setBounds(top.removeFromLeft(190.f));

        area.removeFromTop(padding);

        auto knobs = area.removeFromTop(112.f);
        gain.setBounds(knobs.removeFromLeft(110.f));
        tone.setBounds(knobs.removeFromLeft(110.f));
        mix.setBounds(knobs.removeFromLeft(110.f));

        area.removeFromTop(8.f);

        auto faders = area.removeFromTop(132.f);
        level.setBounds(centredStrip(faders.removeFromLeft(220.f), 70.f));
        faders.removeFromLeft(16.f);
        leftFader.setBounds(faders.removeFromLeft(70.f));
        rightFader.setBounds(faders.removeFromLeft(70.f));

        area.removeFromTop(12.f);

        auto buttons = area.removeFromTop(30.f);
        trigger.setBounds(buttons.removeFromLeft(100.f));
        buttons.removeFromLeft(8.f);
        latch.setBounds(buttons.removeFromLeft(100.f));
        buttons.removeFromLeft(20.f);
        bypass.setBounds(buttons.removeFromLeft(100.f));
        buttons.removeFromLeft(8.f);
        link.setBounds(buttons.removeFromLeft(150.f));

        area.removeFromTop(12.f);

        auto field = area.removeFromTop(28.f);
        editorCaption.setBounds(field.removeFromLeft(70.f));
        editor.setBounds(field.removeFromLeft(260.f));

        // Hard against the bottom edge, which is the whole point of it: there
        // is no room under the box, so the list opens over it instead.
        auto bottom = area.removeFromBottom(26.f);
        program.setBounds(bottom.removeFromLeft(190.f));
        bottom.removeFromLeft(10.f);
        programCaption.setBounds(bottom);
    }

    EventLog log;

    UI::Label presetCaption {"Preset"};
    UI::ComboBox preset {"choose"};

    LabelledControl<UI::Knob> gain {log, "gain", 0.7f};
    LabelledControl<UI::Knob> tone {log, "tone", 0.5f};
    LabelledControl<UI::Knob> mix {log, "mix", 0.35f};

    LabelledControl<UI::Slider> level {log, "level", 0.6f};
    LabelledControl<UI::Slider> leftFader {
        log, "left", 0.8f, UI::Slider::Orientation::Vertical};
    LabelledControl<UI::Slider> rightFader {
        log, "right", 0.8f, UI::Slider::Orientation::Vertical};

    UI::Button trigger {"Trigger"};
    UI::Button latch {"Latch"};
    UI::Checkbox bypass {"Bypass"};
    UI::Checkbox link {"Link channels"};

    UI::Label editorCaption {"Name"};
    UI::TextEditor editor;

    UI::Label programCaption {"40 items at the bottom edge: opens upward, scrolls"};
    UI::ComboBox program {"choose"};
};

// Ten thousand rows that are not stored anywhere: the model is asked for the
// text of the rows on screen and for nothing else.
struct GeneratedRows final : UI::ListBoxModel
{
    int getNumRows() override { return generatedRowCount; }

    void paintRow(UI::Graphics& g,
                  int row,
                  const UI::Rect& bounds,
                  bool selected) override
    {
        const auto& theme = UI::defaultTheme();

        paintRowBackground(g, row, bounds, selected);

        g.setColour(selected ? theme.text : theme.dimText);
        g.drawText(generatedRowText(row), bounds.inset(8.f, 0.f));
    }

    void selectedRowChanged(int row) override { onSelect(row); }

    std::function<void(int)> onSelect = [](int) {};
};

// The same widget with columns, which is the difference between a list and a
// table: the header appears and the model is asked for cells.
struct NoteTable final : UI::ListBoxModel
{
    int getNumRows() override { return 128; }

    void paintRow(UI::Graphics& g,
                  int row,
                  const UI::Rect& bounds,
                  bool selected) override
    {
        paintRowBackground(g, row, bounds, selected);
    }

    void paintCell(UI::Graphics& g,
                   int row,
                   int column,
                   const UI::Rect& bounds,
                   bool selected) override
    {
        const auto& theme = UI::defaultTheme();

        g.setColour(selected ? theme.text : theme.dimText);
        g.drawText(cellText(row, column), bounds.inset(6.f, 0.f));
    }

    std::string cellText(int note, int column) const
    {
        if (column == 0)
            return midiNoteName(note);

        if (column == 1)
            return hertzText(midiNoteHertz(note));

        return std::to_string(note);
    }

    void selectedRowChanged(int row) override { onSelect(row); }

    std::function<void(int)> onSelect = [](int) {};
};

// Reads the host's figures at paint time rather than being told them, and asks
// for one more frame while they are still moving. ComponentTree's footer
// explains why at length; the count to read here is the first one.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto text = std::to_string(host->getLastComponentCount()) + " components   "
                    + std::to_string(host->getLastClipChangeCount())
                    + " batch breaks   "
                    + std::to_string(host->getLastPaintedComponentCount())
                    + " painted   " + std::to_string(generatedRowCount) + " rows";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Right);

        if (text != lastPainted)
        {
            lastPainted = text;
            Threads::callAsync([this] { repaint(); });
        }
    }

    UI::ComponentHost* host = nullptr;
    std::string lastPainted;
};

struct ListPage final : UI::Component
{
    ListPage()
    {
        for (auto* label:
             {&rowsCaption, &notesCaption, &rowsSelection, &notesSelection})
            label->setColour(UI::defaultTheme().dimText);

        rows.onSelect = [this](int row)
        { rowsSelection.setText("selected row " + std::to_string(row)); };

        notes.onSelect = [this](int row)
        {
            notesSelection.setText("selected " + midiNoteName(row) + " at "
                                   + hertzText(midiNoteHertz(row)));
        };

        rowsList.setRowHeight(20.f);
        rowsList.setModel(&rows);
        rowsList.setSelectedRow(0, true);

        notesList.setRowHeight(20.f);
        notesList.setColumns({{"Note", 80.f}, {"Frequency", 130.f}, {"MIDI", 60.f}});
        notesList.setModel(&notes);
        notesList.setSelectedRow(69, true);
        notesList.scrollToRow(69);

        addChildren({rowsCaption, rowsList, rowsSelection});
        addChildren({notesCaption, notesList, notesSelection, stats});
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        stats.setBounds(area.removeFromBottom(20.f));
        area.removeFromBottom(6.f);

        auto left = area.removeFromLeft(area.w * 0.5f - 6.f);
        area.removeFromLeft(padding);

        rowsCaption.setBounds(left.removeFromTop(20.f));
        rowsSelection.setBounds(left.removeFromBottom(20.f));
        rowsList.setBounds(left);

        notesCaption.setBounds(area.removeFromTop(20.f));
        notesSelection.setBounds(area.removeFromBottom(20.f));
        notesList.setBounds(area);
    }

    UI::Label rowsCaption {"Ten thousand rows, one component"};
    UI::Label notesCaption {"The same widget with columns and a header"};
    UI::Label rowsSelection;
    UI::Label notesSelection;

    GeneratedRows rows;
    NoteTable notes;

    UI::ListBox rowsList;
    UI::ListBox notesList;

    StatsBar stats;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        tabs.addTab("Controls", controls);
        tabs.addTab("List", lists);
        tabs.setCurrentTabIndex(startupTab);

        addAndMakeVisible(tabs);
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override { tabs.setBounds(getLocalBounds()); }

    ControlsPage controls;
    ListPage lists;
    UI::TabbedComponent tabs;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.lists.stats.host = this;
        setRootComponent(root);
    }

    DemoRoot root;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1120;
    options.height = 620;
    options.title = "eacp UI — widget gallery";
    options.minWidth = 720;
    options.minHeight = 520;

    return options;
}

struct App
{
    App() { window.setContentView(host); }

    DemoHost host;
    Graphics::Window window {makeOptions()};
};
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        startupTab = static_cast<int>(std::strtol(argv[1], nullptr, 10));

    return eacp::Apps::run<App>();
}
