#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace Graphics;

// Window::getPosition, Window::setPosition and WindowEvents::onMoved, in the
// screen points WindowOptions::initialPosition and Display already speak:
// origin at the primary display's top-left, y growing down.
//
// Drag the window and watch the numbers follow. Press a corner and it goes
// there — to the corner of the *work area*, which is the display minus the
// menu bar and the Dock, so the window lands somewhere the user can still
// reach it.
//
// The pair is what lets an app put a window back where it was left: read
// getPosition on the way out, hand it to WindowOptions::initialPosition on the
// way in (that one also clamps to the display, which setPosition does not).

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
        g.setColor(hovered ? Color::white(0.18f) : Color::white(0.10f));
        g.fillRoundedRect(getLocalBounds(), 7.f);

        g.setColor(Color::white(0.82f));
        g.drawText(text, {12.f, getLocalBounds().h * 0.5f + 4.5f}, font);
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

struct PositionRoot final : View
{
    PositionRoot()
    {
        addChildren({topLeft, topRight, bottomLeft, bottomRight});

        topLeft.setText("Top left");
        topRight.setText("Top right");
        bottomLeft.setText("Bottom left");
        bottomRight.setText("Bottom right");

        topLeft.onClick = [this] { moveToCorner(false, false); };
        topRight.onClick = [this] { moveToCorner(true, false); };
        bottomLeft.onClick = [this] { moveToCorner(false, true); };
        bottomRight.onClick = [this] { moveToCorner(true, true); };
    }

    void setReportedPosition(Point newPosition)
    {
        position = newPosition;
        repaint();
    }

    void resized() override
    {
        auto y = getLocalBounds().h - 56.f;

        topLeft.setBounds({24.f, y, 116.f, 32.f});
        topRight.setBounds({148.f, y, 116.f, 32.f});
        bottomLeft.setBounds({272.f, y, 128.f, 32.f});
        bottomRight.setBounds({408.f, y, 128.f, 32.f});
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.11f, 0.12f, 0.15f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(Color::white(0.94f));
        g.drawText(std::to_string((int) position.x) + ", "
                       + std::to_string((int) position.y),
                   {26.f, 92.f},
                   positionFont);

        g.setColor(Color::white(0.45f));
        g.drawText("window top-left, in screen points — drag the window",
                   {28.f, 124.f},
                   labelFont);
    }

    std::function<void(Point)> onMoveRequested = [](auto&&) {};

private:
    void moveToCorner(bool right, bool bottom)
    {
        auto work = primaryDisplay().workArea;
        auto size = getLocalBounds();

        // The work area is where a window may go; its bottom-right corner is
        // past where a window may *start*, hence the size coming off both.
        auto x = right ? work.x + work.w - size.w : work.x;
        auto y = bottom ? work.y + work.h - size.h : work.y;

        onMoveRequested({x, y});
    }

    Button topLeft;
    Button topRight;
    Button bottomLeft;
    Button bottomRight;
    Point position;

    Font positionFont {FontOptions().withName("Helvetica-Bold").withSize(34.f)};
    Font labelFont {FontOptions().withName("Helvetica").withSize(12.f)};
};

struct WindowPositionApp
{
    WindowPositionApp()
    {
        window.setContentView(root);

        // onMoved lives on WindowEvents rather than WindowOptions precisely so
        // it can be set here, after the window exists — a first position is
        // nothing the constructor needs.
        window.events.onMoved = [this](Point position)
        { root.setReportedPosition(position); };

        // No readout update here: setPosition fires onMoved just as a drag
        // does, so the handler above already has it.
        root.onMoveRequested = [this](Point position)
        { window.setPosition(position); };

        root.setReportedPosition(window.getPosition());
    }

    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.title = "Window Position";
        options.width = 560;
        options.height = 240;
        options.backgroundColor = Color {0.11f, 0.12f, 0.15f, 1.f};

        return options;
    }

    PositionRoot root;
    Window window {getOptions()};
};

int main()
{
    return eacp::Apps::run<WindowPositionApp>();
}
