#include <eacp/Core/App/Clipboard.h>
#include <eacp/UI/UI.h>

#include <string>

using namespace eacp;

namespace
{
constexpr auto backgroundColour = Graphics::Color {0.11f, 0.12f, 0.15f};
constexpr auto rowHeight = 22.f;
constexpr auto maxRows = 18;

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

// Control characters would draw as boxes, so they are escaped.
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

struct InspectorContent final : UI::Component
{
    InspectorContent()
    {
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(true);

        rows.push_back("Press any key. Cmd+C copies this log, Cmd+V pastes.");
    }

    // Consumes every key because it is the inspector; a component that wants
    // only some must return false for the rest, or shortcuts die in it.
    bool keyDown(const UI::KeyEvent& event) override
    {
        if (event.modifiers.command && event.charactersIgnoringModifiers == "c")
        {
            auto joined = std::string {};

            for (const auto& row: rows)
                joined += row + "\n";

            const auto copied = Clipboard::copyText(joined);
            add(copied ? "-- copied the log to the clipboard" : "-- copy failed");

            return true;
        }

        if (event.modifiers.command && event.charactersIgnoringModifiers == "v")
        {
            // Asked first, so "nothing to paste" stays distinguishable from an
            // empty clipboard read.
            if (!Clipboard::hasText())
            {
                add("-- clipboard holds no text");
                return true;
            }

            const auto text = Clipboard::getText();
            add("-- pasted " + std::to_string(text.size()) + " bytes: \""
                + printable(text.substr(0, 40)) + "\"");

            return true;
        }

        add(nameFor(event.keyCode) + "   chars: " + printable(event.characters)
            + "   raw: " + printable(event.charactersIgnoringModifiers) + "   mods: "
            + modifiersOf(event.modifiers) + (event.isRepeat ? "   (repeat)" : ""));

        return true;
    }

    void add(std::string row)
    {
        rows.push_back(std::move(row));

        while ((int) rows.size() > maxRows)
            rows.erase(rows.begin());

        repaint();
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(backgroundColour);

        g.setFont({Text::defaultMonospaceFamily(), 13.f});

        auto y = 16.f;

        for (const auto& row: rows)
        {
            const auto isNote = !row.empty() && row[0] == '-';

            g.setColour(isNote ? UI::Color {0.55f, 0.80f, 0.60f, 1.f}
                               : UI::Color {0.85f, 0.87f, 0.91f, 1.f});

            g.drawText(row, {16.f, y + g.ascent()});
            y += rowHeight;
        }
    }

    std::vector<std::string> rows;
};

struct InspectorHost final : UI::ComponentHost
{
    InspectorHost()
    {
        setBackgroundColour(backgroundColour);

        // Tab is a key to report here, not a way out of the component.
        setTabMovesFocus(false);

        setRootComponent(content);
        content.grabKeyboardFocus();
    }

    InspectorContent content;
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 720;
    options.height = 460;
    options.title = "Key Inspector";
    options.backgroundColor = backgroundColour;

    return options;
}

struct KeyInspectorApp
{
    KeyInspectorApp()
    {
        window.setContentView(host);

        // The native view must be the window's first responder before the tree
        // hears any key at all.
        host.focus();
    }

    InspectorHost host;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<KeyInspectorApp>();
}
