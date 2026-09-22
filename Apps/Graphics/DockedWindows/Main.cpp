#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace Graphics;

// WindowOptions::parent and Window::setParent: a tool palette docked to a
// document window, which is the shape every editor, DAW and paint program
// has.
//
// Drag the document window and the palette keeps its offset. Hide the
// document and the palette goes with it. Close the document and both go: a
// child never quits the app on its own, so the palette's close does nothing
// to the process, and the document's takes the palette down with it.
//
// The palette opens beside its parent through WindowOptions::parentOffset,
// which is measured from the parent's frame top-left — initialPosition is
// screen-absolute, and would have to be computed from wherever the document
// window happened to land.

struct Swatch final : View
{
    Swatch()
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    void paint(Context& g) override
    {
        g.setColor(color);
        g.fillRoundedRect(getLocalBounds(), 6.f);

        if (!hovered)
            return;

        g.setColor(Color::white(0.85f));
        g.setLineWidth(2.f);
        g.strokeRect(getLocalBounds().inset(1.f));
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
            onPicked(color);
    }

    std::function<void(Color)> onPicked = [](auto&&) {};

    Color color = Color::white(1.f);
    bool hovered = false;
};

struct TextButton final : View
{
    TextButton()
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    void setText(std::string newText)
    {
        text = newText;
        repaint();
    }

    void paint(Context& g) override
    {
        g.setColor(hovered ? Color::white(0.22f) : Color::white(0.12f));
        g.fillRoundedRect(getLocalBounds(), 7.f);

        g.setColor(Color::white(0.85f));
        g.drawText(text, {14.f, getLocalBounds().h * 0.5f + 4.5f}, font);
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
    Font font {FontOptions().withName("Helvetica-Bold").withSize(12.f)};
    bool hovered = false;
};

struct PaletteRoot final : View
{
    PaletteRoot()
    {
        addChildren({swatches[0], swatches[1], swatches[2], swatches[3]});

        auto colors = std::initializer_list<Color> {{0.92f, 0.36f, 0.33f, 1.f},
                                                    {0.35f, 0.70f, 0.45f, 1.f},
                                                    {0.34f, 0.55f, 0.92f, 1.f},
                                                    {0.92f, 0.78f, 0.31f, 1.f}};

        auto index = 0;

        for (auto& color: colors)
        {
            swatches[index].color = color;
            swatches[index].onPicked = [this](Color picked) { onPicked(picked); };
            ++index;
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(14.f);

        for (auto& swatch: swatches)
            swatch.setBounds(area.removeFromTop(46.f).withHeight(38.f));
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.16f, 0.17f, 0.20f, 1.f});
        g.fillRect(getLocalBounds());
    }

    std::function<void(Color)> onPicked = [](auto&&) {};

    Swatch swatches[4];
};

struct DocumentRoot final : View
{
    DocumentRoot()
    {
        addChildren({dockToggle});
        dockToggle.setText("Undock the palette");
    }

    void setColor(Color newColor)
    {
        color = newColor;
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(24.f);
        dockToggle.setBounds(area.fromBottom(34.f).withWidth(180.f));
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.11f, 0.12f, 0.15f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(color);
        g.fillRoundedRect(getLocalBounds().inset(24.f).fromTop(120.f), 10.f);

        g.setColor(Color::white(0.55f));
        g.drawText(
            "Drag this window — the palette keeps its offset", {26.f, 176.f}, font);
    }

    TextButton dockToggle;
    Color color {0.34f, 0.55f, 0.92f, 1.f};
    Font font {FontOptions().withName("Helvetica").withSize(12.f)};
};

struct DockedWindowsApp
{
    DockedWindowsApp()
    {
        palette.onPicked = [this](Color picked) { document.setColor(picked); };
        document.dockToggle.onClick = [this] { toggleDocking(); };
    }

    void toggleDocking()
    {
        auto docked = paletteWindow.getParent() != nullptr;

        paletteWindow.setParent(docked ? nullptr : &window);
        document.dockToggle.setText(docked ? "Dock the palette"
                                           : "Undock the palette");
    }

    static WindowOptions documentOptions()
    {
        auto options = WindowOptions {};

        options.title = "Document";
        options.width = 520;
        options.height = 300;
        options.initialPosition = Point {180.f, 140.f};
        options.backgroundColor = Color {0.11f, 0.12f, 0.15f, 1.f};

        return options;
    }

    // The palette is the child: above the document and only above it, carried
    // with it, ordered out with it, and closed with it. A child never quits
    // the app whatever isPrimary says — but the button below undocks this one,
    // and an undocked window is an ordinary one again (Window::quitsApp is the
    // live answer, not the one its options were written with), so the palette
    // says for itself that closing it is not what ends the app.
    WindowOptions paletteOptions()
    {
        auto options = WindowOptions {};

        options.title = "Tools";
        options.width = 96;
        options.height = 224;
        options.isPrimary = false;
        options.parent = &window;
        options.parentOffset = Point {536.f, 0.f};
        options.backgroundColor = Color {0.16f, 0.17f, 0.20f, 1.f};
        options.flags = {WindowFlags::Titled, WindowFlags::Closable};

        return options;
    }

    DocumentRoot document;
    Window window {document, documentOptions()};

    PaletteRoot palette;
    Window paletteWindow {palette, paletteOptions()};
};

int main()
{
    return eacp::Apps::run<DockedWindowsApp>();
}
