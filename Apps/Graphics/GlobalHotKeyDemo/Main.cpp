#include <eacp/UI/UI.h>

using namespace eacp;
using namespace Graphics;

constexpr auto chordName = "Command (Windows key) + Space";
constexpr auto chordModifiers = ModifierKeys {.command = true};
constexpr auto chordKeyCode = KeyCode::Space;

std::string describeAvailability()
{
    auto isFree = GlobalHotKey::isAvailable(chordModifiers, chordKeyCode);

    if (isFree)
        return "isAvailable says: free";

    return "isAvailable says: taken by the system";
}

std::string describePresses(int pressCount)
{
    return "Bound a GlobalHotKey to it. Presses this app received: "
           + std::to_string(pressCount);
}

struct HotKeyContent final : UI::Component
{
    HotKeyContent()
    {
        addChildren({chordLine, availabilityLine, pressesLine, explanationLine});
    }

    void resized() override
    {
        constexpr auto margin = 24.f;
        constexpr auto lineHeight = 28.f;

        auto area = getLocalBounds().inset(margin, margin);

        for (auto* line:
             {&chordLine, &availabilityLine, &pressesLine, &explanationLine})
            line->setBounds(area.removeFromTop(lineHeight));
    }

    void countPress()
    {
        ++pressCount;
        pressesLine.setText(describePresses(pressCount));
    }

    int pressCount = 0;

    UI::Label chordLine {std::string {"Chord: "} + chordName};
    UI::Label availabilityLine {describeAvailability()};
    UI::Label pressesLine {describePresses(pressCount)};
    UI::Label explanationLine {
        "Press it: the system's shortcut wins, so the count stays at 0."};

    GlobalHotKey hotKey {chordModifiers, chordKeyCode, [this] { countPress(); }};
};

struct HotKeyHost final : UI::ComponentHost
{
    HotKeyHost() { setRootComponent(content); }

    HotKeyContent content;
};

static WindowOptions getWindowOptions()
{
    auto options = WindowOptions();
    options.width = 620;
    options.height = 180;
    options.title = "Global Hot Key Demo";
    return options;
}

struct GlobalHotKeyDemo
{
    HotKeyHost host;
    Window window {host, getWindowOptions()};
};

int main()
{
    return Apps::run<GlobalHotKeyDemo>();
}
