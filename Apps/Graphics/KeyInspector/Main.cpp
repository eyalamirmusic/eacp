#include <eacp/Core/App/Clipboard.h>
#include <eacp/Graphics/Graphics.h>

#include <string>
#include <vector>

// Key events and the clipboard, made visible.
//
// Press anything: the key's code, the characters it produced with and without
// modifiers, and which modifiers were held all appear. That is the whole
// KeyEvent contract on screen at once, which is the quickest way to answer
// "what does this key actually report on this platform" — the question that
// otherwise gets answered by guessing at raw virtual key codes.
//
// Cmd+C copies the log, Cmd+V pastes the clipboard into it, so the round trip
// is exercised by hand as well as by the tests.

using namespace eacp;

namespace
{
constexpr auto background = Graphics::Color {0.11f, 0.12f, 0.15f};
constexpr auto rowHeight = 22.f;
constexpr auto maxRows = 18;

// Names for the keys worth naming. Anything else shows as its numeric code,
// which is still useful — it is what you would put in a table.
std::string nameFor(std::uint16_t code)
{
    struct Named
    {
        std::uint16_t code;
        const char* name;
    };

    static const Named names[] = {
        {Graphics::KeyCode::Space, "Space"},
        {Graphics::KeyCode::Return, "Return"},
        {Graphics::KeyCode::Tab, "Tab"},
        {Graphics::KeyCode::Delete, "Delete (backspace)"},
        {Graphics::KeyCode::ForwardDelete, "ForwardDelete"},
        {Graphics::KeyCode::Escape, "Escape"},
        {Graphics::KeyCode::LeftArrow, "LeftArrow"},
        {Graphics::KeyCode::RightArrow, "RightArrow"},
        {Graphics::KeyCode::UpArrow, "UpArrow"},
        {Graphics::KeyCode::DownArrow, "DownArrow"},
        {Graphics::KeyCode::Home, "Home"},
        {Graphics::KeyCode::End, "End"},
        {Graphics::KeyCode::PageUp, "PageUp"},
        {Graphics::KeyCode::PageDown, "PageDown"},
        {Graphics::KeyCode::Minus, "Minus"},
        {Graphics::KeyCode::Equals, "Equals"},
        {Graphics::KeyCode::LeftBracket, "LeftBracket"},
        {Graphics::KeyCode::RightBracket, "RightBracket"},
        {Graphics::KeyCode::Backslash, "Backslash"},
        {Graphics::KeyCode::Semicolon, "Semicolon"},
        {Graphics::KeyCode::Quote, "Quote"},
        {Graphics::KeyCode::Comma, "Comma"},
        {Graphics::KeyCode::Period, "Period"},
        {Graphics::KeyCode::Slash, "Slash"},
        {Graphics::KeyCode::Grave, "Grave"},
        {Graphics::KeyCode::KeypadEnter, "KeypadEnter"},
        {Graphics::KeyCode::CapsLock, "CapsLock"},
    };

    for (const auto& named: names)
        if (named.code == code)
            return named.name;

    return "code " + std::to_string(code);
}

std::string modifiersOf(const Graphics::ModifierKeys& modifiers)
{
    auto held = std::string {};

    const auto add = [&held](bool on, const char* name)
    {
        if (!on)
            return;

        if (!held.empty())
            held += "+";

        held += name;
    };

    add(modifiers.command, "Cmd");
    add(modifiers.control, "Ctrl");
    add(modifiers.alt, "Alt");
    add(modifiers.shift, "Shift");

    return held.empty() ? "-" : held;
}

// Control characters would draw as boxes, so they are shown by name.
std::string printable(const std::string& text)
{
    if (text.empty())
        return "-";

    auto result = std::string {};

    for (const auto character: text)
    {
        if (static_cast<unsigned char>(character) < 0x20)
            result += "\\x" + std::to_string(static_cast<int>(character));
        else
            result += character;
    }

    return result;
}

struct InspectorView final : Graphics::View
{
    InspectorView()
    {
        setHandlesMouseEvents(true);
        setGrabsFocusOnMouseDown(true);

        rows.push_back("Press any key. Cmd+C copies this log, Cmd+V pastes.");
    }

    void keyDown(const Graphics::KeyEvent& event) override
    {
        if (event.modifiers.command
            && event.charactersIgnoringModifiers == "c")
        {
            auto joined = std::string {};

            for (const auto& row: rows)
                joined += row + "\n";

            const auto copied = Clipboard::copyText(joined);
            add(copied ? "-- copied the log to the clipboard"
                       : "-- copy failed");
            return;
        }

        if (event.modifiers.command
            && event.charactersIgnoringModifiers == "v")
        {
            // hasText first, so the "nothing to paste" case is distinguishable
            // from an empty clipboard read.
            if (!Clipboard::hasText())
            {
                add("-- clipboard holds no text");
                return;
            }

            const auto text = Clipboard::getText();
            add("-- pasted " + std::to_string(text.size()) + " bytes: \""
                + printable(text.substr(0, 40)) + "\"");
            return;
        }

        add(nameFor(event.keyCode) + "   chars: " + printable(event.characters)
            + "   raw: " + printable(event.charactersIgnoringModifiers)
            + "   mods: " + modifiersOf(event.modifiers)
            + (event.isRepeat ? "   (repeat)" : ""));
    }

    void add(std::string row)
    {
        rows.push_back(std::move(row));

        while (rows.size() > maxRows)
            rows.erase(rows.begin());

        repaint();
    }

    void paint(Graphics::Context& context) override
    {
        const auto bounds = getLocalBounds();

        context.setColor(background);
        context.fillRect(bounds);

        const auto font = Graphics::Font {{"Menlo", 13.f}};
        auto y = 28.f;

        for (const auto& row: rows)
        {
            const auto isNote = !row.empty() && row[0] == '-';

            context.setColor(isNote ? Graphics::Color {0.55f, 0.80f, 0.60f}
                                    : Graphics::Color {0.85f, 0.87f, 0.91f});

            context.drawText(row, {16.f, y}, font);
            y += rowHeight;
        }
    }

    std::vector<std::string> rows;
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 720;
    options.height = 460;
    options.title = "Key Inspector";
    options.backgroundColor = background;

    return options;
}

struct KeyInspectorApp
{
    KeyInspectorApp()
    {
        window.setContentView(view);
        view.focus();
    }

    InspectorView view;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<KeyInspectorApp>();
}
