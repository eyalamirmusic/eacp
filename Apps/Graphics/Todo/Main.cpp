#include <eacp/UI/UI.h>

#include <string>

using namespace eacp;

namespace
{
constexpr auto rowHeight = 40.f;
constexpr auto padding = 10.f;

struct TodoRow final : UI::Component
{
    explicit TodoRow(std::string textToUse)
        : itemText(std::move(textToUse))
    {
        setInterceptsMouseClicks(true);

        setWantsKeyboardFocus(true);

        tick.setText(itemText);
        tick.onChange = [this](bool) { refresh(); };

        editor.setText(itemText);

        // Losing focus commits too, which is what clicking away from a
        // half-edited field should mean.
        editor.onReturnKey = [this](const std::string& value)
        { finishEditing(value); };
        editor.onEscapeKey = [this] { finishEditing(itemText); };

        addAndMakeVisible(tick);
        addChildComponent(editor);

        refresh();
    }

    void startEditing()
    {
        if (editing)
            return;

        editing = true;

        tick.setVisible(false);
        editor.setVisible(true);
        editor.setText(itemText);
        editor.selectAll();
        editor.grabKeyboardFocus();

        resized();
        repaint();
    }

    void finishEditing(const std::string& newText)
    {
        if (!editing)
            return;

        editing = false;
        itemText = newText;

        editor.setVisible(false);
        tick.setVisible(true);
        tick.setText(itemText);

        // Back to the row, so the keyboard is not left on a hidden component.
        grabKeyboardFocus();

        refresh();
        resized();
        repaint();
    }

    void refresh()
    {
        tick.setAccentColour({0.3f, 0.8f, 0.4f, 1.f});
        repaint();
    }

    void mouseDown(const UI::MouseEvent& event) override
    {
        if (event.clickCount == 2)
            startEditing();
    }

    void paint(UI::Graphics& g) override
    {
        if (!isMouseOver() && !editing)
            return;

        g.setColour(UI::defaultTheme().hover);
        g.fillRoundedRect(getLocalBounds(), 6.f);
    }

    void mouseEnter(const UI::MouseEvent&) override { repaint(); }
    void mouseExit(const UI::MouseEvent&) override { repaint(); }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding, 6.f);

        if (editing)
            editor.setBounds(area);
        else
            tick.setBounds(area);
    }

    // Struck through when done, which the checkbox's own caption cannot do.
    void paintOverChildren(UI::Graphics& g) override
    {
        if (editing || !tick.isChecked())
            return;

        auto captionLeft = padding + 26.f;
        auto width = measureText(itemText, getHostFont());

        g.setColour(UI::defaultTheme().dimText);
        g.drawLine({captionLeft, getHeight() * 0.5f},
                   {captionLeft + width, getHeight() * 0.5f},
                   1.f);
    }

    bool editing = false;
    std::string itemText;

    UI::Checkbox tick;
    UI::TextEditor editor;
};

struct TodoList final : UI::Component
{
    TodoList()
    {
        title.setFontSize(22.f);
        title.setFontStyle(UI::FontStyle::Bold);

        hint.setColour(UI::defaultTheme().dimText);

        addChildren({title, hint});

        for (auto* text: {"Learn eacp framework",
                          "Build a todo app",
                          "Add more features",
                          "Test the application",
                          "Ship it!"})
        {
            auto& row = rows.createNew(text);
            addAndMakeVisible(row);
        }
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll({0.08f, 0.08f, 0.1f, 1.f});

        g.setColour({0.15f, 0.15f, 0.15f, 1.f});
        g.fillRoundedRect(getLocalBounds().getRelative({0.05f, 0.05f, 0.9f, 0.9f}),
                          12.f);
    }

    void resized() override
    {
        auto area =
            getLocalBounds().getRelative({0.05f, 0.05f, 0.9f, 0.9f}).inset(20.f);

        title.setBounds(area.removeFromTop(34.f));
        hint.setBounds(area.removeFromTop(22.f));
        area.removeFromTop(12.f);

        for (auto& row: rows)
            row->setBounds(area.removeFromTop(rowHeight));
    }

    UI::Label title {"Todo List"};
    UI::Label hint {"Click to check off · double-click to edit · Tab to move"};
    OwnedVector<TodoRow> rows;
};

struct Host final : UI::ComponentHost
{
    Host()
    {
        setBackgroundColour({0.08f, 0.08f, 0.1f, 1.f});
        setRootComponent(list);
    }

    TodoList list;
};

struct TodoApp
{
    TodoApp()
    {
        window.setContentView(host);
        host.focus();
    }

    Host host;
    eacp::Graphics::Window window;
};
} // namespace

int main()
{
    return eacp::Apps::run<TodoApp>();
}
