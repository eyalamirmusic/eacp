#include <eacp/Graphics/Graphics.h>

using namespace eacp;
using namespace Graphics;

// Apps::setAppBadge — the unread count on the Dock tile (macOS) or over the
// taskbar button (Windows). Add messages by hand, or let the timer bring them
// in the way the app it is meant for would, and watch the badge follow.
//
// Threads::Timer takes a rate in Hz, not a period, so 1 is one message a
// second.
//
// The badge is text, not a number, so what an app puts there is its own
// decision: this one caps at 99+ the way a mail client does, and clears with
// an empty string.

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

struct BadgeRoot final : View
{
    BadgeRoot()
    {
        addChildren({addOne, addTen, clear, autoIncoming});

        addOne.setText("One more message");
        addOne.onClick = [this] { setUnread(unread + 1); };

        addTen.setText("Ten more");
        addTen.onClick = [this] { setUnread(unread + 10); };

        clear.setText("Mark all read");
        clear.onClick = [this] { setUnread(0); };

        autoIncoming.onClick = [this] { toggleIncoming(); };

        updateIncomingText();
        applyBadge();
    }

    void resized() override
    {
        auto y = getLocalBounds().h - 62.f;

        addOne.setBounds({28.f, y, 190.f, 36.f});
        addTen.setBounds({228.f, y, 120.f, 36.f});
        clear.setBounds({358.f, y, 150.f, 36.f});
        autoIncoming.setBounds({518.f, y, 190.f, 36.f});
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.11f, 0.12f, 0.15f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(Color::white(0.92f));
        g.drawText(badgeText().empty() ? "no badge" : badgeText(),
                   {30.f, 132.f},
                   countFont);

        g.setColor(Color::white(0.45f));
        g.drawText(std::to_string(unread)
                       + " unread — look at the Dock tile "
                         "(macOS) or the taskbar button "
                         "(Windows)",
                   {32.f, 168.f},
                   labelFont);
    }

private:
    void setUnread(int count)
    {
        unread = std::max(0, count);
        applyBadge();
        repaint();
    }

    // What an app decides to show: a count that stops being worth reading past
    // two digits, and nothing at all at zero.
    std::string badgeText() const
    {
        if (unread <= 0)
            return {};

        return unread > 99 ? std::string {"99+"} : std::to_string(unread);
    }

    void applyBadge() { Apps::setAppBadge(badgeText()); }

    void toggleIncoming()
    {
        if (incoming)
            incoming.reset();
        else
            incoming.emplace([this] { setUnread(unread + 1); }, 1);

        updateIncomingText();
    }

    void updateIncomingText()
    {
        autoIncoming.setText(incoming ? "Stop the messages" : "One a second");
    }

    Button addOne;
    Button addTen;
    Button clear;
    Button autoIncoming;
    std::optional<Threads::Timer> incoming;
    int unread = 0;

    Font countFont {FontOptions().withName("Helvetica-Bold").withSize(40.f)};
    Font labelFont {FontOptions().withName("Helvetica").withSize(12.f)};
};

struct AppBadgeApp
{
    AppBadgeApp() { window.setContentView(root); }

    // The badge outlives nothing: an app that leaves one behind on exit has
    // lied to the Dock, which keeps drawing it until the tile is rebuilt.
    ~AppBadgeApp() { Apps::setAppBadge(""); }

    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.title = "App Badge";
        options.width = 740;
        options.height = 300;
        options.backgroundColor = Color {0.11f, 0.12f, 0.15f, 1.f};

        return options;
    }

    BadgeRoot root;
    Window window {getOptions()};
};

int main()
{
    return eacp::Apps::run<AppBadgeApp>();
}
