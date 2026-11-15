#include <eacp/Graphics/Graphics.h>

#include <thread>

using namespace eacp;
using namespace Graphics;

// Apps::showAlert, one button per kind of alert: each icon, a choice with a
// cancel button, more buttons than a message box has, one raised from a worker
// thread, and two raised at once. The button pressed is written beside them,
// so Return, Escape and the mouse can each be checked against what the options
// asked for.

struct Button final : View
{
    Button()
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    void setText(std::string newText)
    {
        text = std::move(newText);
        repaint();
    }

    void paint(Context& g) override
    {
        g.setColor(hovered ? Color::white(0.18f) : Color::white(0.11f));
        g.fillRoundedRect(getLocalBounds(), 8.f);

        g.setColor(Color::white(0.85f));
        g.drawText(text, {14.f, getLocalBounds().h * 0.5f + 5.f}, font);
    }

    void mouseEntered(const MouseEvent&) override
    {
        hovered = true;
        repaint();
    }

    void mouseExited(const MouseEvent&) override
    {
        hovered = false;
        repaint();
    }

    void mouseUp(const MouseEvent& event) override
    {
        if (getLocalBounds().contains(event.pos))
            onClick();
    }

    std::function<void()> onClick = [] {};

    std::string text;
    Font font {FontOptions().withName("Helvetica-Bold").withSize(13.f)};
    bool hovered = false;
};

struct Demo
{
    std::string label;
    Apps::AlertOptions options;
    bool fromWorkerThread = false;
    int count = 1;
};

Apps::AlertOptions alertOptions(std::string title,
                                std::string message,
                                Apps::AlertIcon icon,
                                Vector<std::string> buttons = {},
                                int defaultButton = 0,
                                std::optional<int> cancelButton = {})
{
    auto options = Apps::AlertOptions {};
    options.title = std::move(title);
    options.message = std::move(message);
    options.icon = icon;
    options.buttons = std::move(buttons);
    options.defaultButton = defaultButton;
    options.cancelButton = cancelButton;
    return options;
}

Vector<Demo> demos()
{
    using Apps::AlertIcon;

    return {
        {"Info",
         alertOptions("Export finished",
                      "The file was written to your Documents folder.",
                      AlertIcon::info)},
        {"Warning, three buttons",
         alertOptions("Do you want to save the changes?",
                      "Your changes will be lost if you don't save them. Return "
                      "saves, Escape cancels.",
                      AlertIcon::warning,
                      {"Save", "Don't Save", "Cancel"},
                      0,
                      2)},
        {"Error",
         alertOptions("The file could not be saved",
                      "The disk is full. Free some space and try again.",
                      AlertIcon::error)},
        {"Default is the second",
         alertOptions("Delete the preset?",
                      "Return presses Keep, the second button; there is no "
                      "cancel button, so Escape does nothing.",
                      AlertIcon::warning,
                      {"Delete", "Keep"},
                      1)},
        {"Default is also cancel",
         alertOptions("Discard the recording?",
                      "Keep is both the default and the cancel button: Return and "
                      "Escape both answer 1.",
                      AlertIcon::warning,
                      {"Discard", "Keep"},
                      1,
                      1)},
        {"Five buttons",
         alertOptions("Pick one",
                      "More buttons than a message box has.",
                      AlertIcon::info,
                      {"One", "Two", "Three", "Four", "Five"},
                      2,
                      4)},
        {"From a worker thread",
         alertOptions("Raised off the main thread",
                      "showAlert was called from a worker; this is on the main "
                      "thread all the same.",
                      AlertIcon::info),
         true},
        {"Two in a row",
         alertOptions("One of two",
                      "Two alerts raised at once come up one after the other. Quit "
                      "(Cmd+Q) while this one is up: both are answered with the "
                      "cancel button, 0, and the app quits.",
                      AlertIcon::warning,
                      {"Keep", "Delete"},
                      1,
                      0),
         false,
         2},
    };
}

struct AlertRoot final : View
{
    AlertRoot()
    {
        for (auto& demo: all)
        {
            auto& button = buttons.createNew();
            button.setText(demo.label);
            button.onClick = [this, &demo] { show(demo); };
            addSubview(button);
        }
    }

    void resized() override
    {
        auto y = 24.f;

        for (auto& button: buttons)
        {
            button->setBounds({28.f, y, 240.f, 36.f});
            y += 46.f;
        }
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.11f, 0.12f, 0.15f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(Color::white(0.92f));
        g.drawText(lastAnswer, {300.f, 52.f}, answerFont);
    }

private:
    void show(const Demo& demo)
    {
        auto onDismissed = [this, demo](int button)
        {
            auto& names = demo.options.buttons;
            auto name = button < names.size() ? names[button] : std::string {"OK"};

            lastAnswer = demo.label + ": pressed " + std::to_string(button) + " ("
                         + name + ")";
            repaint();
        };

        if (!demo.fromWorkerThread)
        {
            for (auto shown = 0; shown < demo.count; ++shown)
                Apps::showAlert(demo.options, onDismissed);

            return;
        }

        std::thread([options = demo.options, onDismissed]
                    { Apps::showAlert(options, onDismissed); })
            .join();
    }

    Vector<Demo> all = demos();
    OwnedVector<Button> buttons;
    std::string lastAnswer = "Nothing pressed yet";
    Font answerFont {FontOptions().withName("Helvetica").withSize(14.f)};
};

struct AlertDemoApp
{
    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.title = "Alert Demo";
        options.width = 760;
        options.height = 420;
        options.backgroundColor = Color {0.11f, 0.12f, 0.15f, 1.f};

        return options;
    }

    AlertRoot root;
    Window window {root, getOptions()};
};

int main()
{
    return eacp::Apps::run<AlertDemoApp>();
}
