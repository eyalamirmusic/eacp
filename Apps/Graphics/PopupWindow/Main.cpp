#include "ForeignView.h"

#include <eacp/Graphics/Graphics.h>

#include <eacp/Core/Threads/EventLoop.h>

#include <ctime>
#include <memory>

// A popup Window over foreign native content: the menu a plugin host has to be
// able to draw over a plugin's editor.
//
// The editor below the toolbar is a real view of another toolkit, parented
// into a NativeChildSurface, and it has a button of its own that prints when
// it is pressed. An eacp View laid over it would lose: a foreign native child
// sits above our own drawing on Windows and X11, and on macOS it keeps its own
// tracking areas and its implicit mouse-down grab whatever is painted on top.
// A popup Window is a separate native window, owned by this one and levelled
// above it, which is the only thing that covers it.
//
// What is worth watching while it runs:
//   - the menu draws over the foreign view, which shows through the panel's
//     translucent body rather than only around it;
//   - its items highlight under the pointer although the popup is never key;
//   - the main window's title bar stays active the whole time;
//   - a click outside dismisses the menu AND the plugin's button does not
//     fire, because the press that closed the menu belonged to the menu;
//   - Escape dismisses it, and dragging the main window carries it along;
//   - every open builds a different list, so the popup is a different size
//     each time — there is no cached window being shown again.

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto toolbarHeight = 52.f;
constexpr auto itemHeight = 28.f;
constexpr auto menuPadding = 6.f;
constexpr auto menuWidth = 260.f;
constexpr auto menuCorner = 8.f;

// The window is transparentBackground, so the panel is the whole of what is
// drawn: its body is see-through and the plugin's editor shows through it,
// and the rounded shape the system used to cut is cut here instead.
constexpr auto panelOpacity = 0.80f;

// Higher than the body, so a hovered item reads as a solid bar over a
// see-through panel rather than as one more layer of it.
constexpr auto highlightOpacity = 0.90f;

std::string currentTimeText()
{
    auto now = std::time(nullptr);
    char text[16] {};
    std::strftime(text, sizeof(text), "%H:%M:%S", std::localtime(&now));

    return text;
}

// Built again on every open, and deliberately a different length each time:
// a popup whose size is decided by its content is the case a cached window
// quietly gets wrong.
Vector<std::string> buildMenuItems(int openCount)
{
    auto items = Vector<std::string> {};

    items.add("Opened at " + currentTimeText());
    items.add("Open #" + std::to_string(openCount));

    for (auto index = 0; index < 1 + openCount % 4; ++index)
        items.add("Generated entry " + std::to_string(index + 1));

    items.add("Close this menu");

    return items;
}

struct MenuItem final : View
{
    explicit MenuItem(const std::string& textToUse)
        : text(textToUse)
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    void paint(Context& g) override
    {
        if (hovered)
        {
            g.setColor({0.20f, 0.42f, 0.78f, highlightOpacity});
            g.fillRoundedRect(getLocalBounds(), 5.f);
        }

        g.setColor(Color::white());
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
            onChosen(text);
    }

    std::function<void(const std::string&)> onChosen = [](auto&&) {};

    std::string text;
    Font font {FontOptions().withName("Helvetica").withSize(13.f)};
    bool hovered = false;
};

struct MenuContent final : View
{
    void setItems(const Vector<std::string>& texts)
    {
        items.clear();

        for (const auto& text: texts)
        {
            auto& item = items.createNew(text);
            item.onChosen = [this](const std::string& chosen) { onChosen(chosen); };
            addSubview(item);
        }

        resized();
    }

    Point preferredSize() const
    {
        return {menuWidth, menuPadding * 2.f + itemHeight * (float) items.size()};
    }

    void paint(Context& g) override
    {
        g.setColor({0.13f, 0.14f, 0.16f, panelOpacity});
        g.fillRoundedRect(getLocalBounds(), menuCorner);

        auto outline = Path {};
        outline.addRoundedRect(getLocalBounds().inset(0.5f), menuCorner);

        g.setColor(Color::white(0.16f));
        g.setLineWidth(1.f);
        g.strokePath(outline);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(menuPadding);

        for (auto& item: items)
            item->setBounds(area.removeFromTop(itemHeight));
    }

    std::function<void(const std::string&)> onChosen = [](auto&&) {};

    OwnedVector<MenuItem> items;
};

struct Button final : View
{
    explicit Button(const std::string& textToUse)
        : text(textToUse)
    {
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    void paint(Context& g) override
    {
        g.setColor(hovered ? Color::white(0.22f) : Color::white(0.12f));
        g.fillRoundedRect(getLocalBounds(), 6.f);

        g.setColor(Color::white(0.88f));
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

    void mouseDown(const MouseEvent&) override { onClick(); }

    std::function<void()> onClick = [] {};

    std::string text;
    Font font {FontOptions().withName("Helvetica-Bold").withSize(12.f)};
    bool hovered = false;
};

struct Toolbar final : View
{
    Toolbar()
    {
        setHandlesMouseEvents(true);
        addSubview(menuButton);

        menuButton.onClick = [this]
        {
            auto below = Point {0.f, menuButton.getLocalBounds().h + 4.f};
            onMenuRequested(menuButton.localToScreen(below));
        };
    }

    void paint(Context& g) override
    {
        g.setColor({0.18f, 0.19f, 0.21f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(Color::white(0.55f));
        g.drawText("Right-click anywhere in this strip, or press Menu",
                   {160.f, getLocalBounds().h * 0.5f + 4.5f},
                   font);
    }

    void resized() override
    {
        menuButton.setBounds(getLocalBounds().inset(12.f, 11.f).fromLeft(120.f));
    }

    // Right anywhere, which is what a mixer strip does: the menu opens under
    // the pointer rather than under a control.
    void mouseDown(const MouseEvent& event) override
    {
        if (event.button == MouseButton::Right)
            onMenuRequested(localToScreen(event.pos));
    }

    std::function<void(Point)> onMenuRequested = [](auto&&) {};

    Button menuButton {"Menu"};
    Font font {FontOptions().withName("Helvetica").withSize(12.f)};
};

struct PopupDemo final : View
{
    PopupDemo()
    {
        addChildren({toolbar, pluginArea});

        toolbar.onMenuRequested = [this](Point screenPosition)
        { openMenu(screenPosition); };

        menu.onChosen = [this](const std::string& chosen)
        {
            std::printf("[menu] chose \"%s\"\n", chosen.c_str());
            std::fflush(stdout);
            closeMenu();
        };
    }

    void paint(Context& g) override
    {
        g.setColor({0.09f, 0.09f, 0.10f, 1.f});
        g.fillRect(getLocalBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        toolbar.setBounds(area.removeFromTop(toolbarHeight));
        pluginArea.setBounds(area);

        attachForeignContent();
    }

private:
    // Once the surface is in a window, which is when its native handle
    // exists — on Windows a child window needs a parent window to be created
    // at all.
    void attachForeignContent()
    {
        if (foreignContentAttached || getWindow() == nullptr)
            return;

        if (auto* handle = pluginArea.getNativeParentHandle())
        {
            addForeignContent(handle);
            foreignContentAttached = true;
        }
    }

    void openMenu(Point screenPosition)
    {
        // The old one first: one menu at a time, and its content view is
        // about to be adopted by the new window.
        popup.reset();

        ++openCount;
        menu.setItems(buildMenuItems(openCount));

        auto size = menu.preferredSize();

        auto options = WindowOptions {};
        options.popup = true;
        options.parent = getWindow();
        options.title = "Menu";
        options.width = (int) size.x;
        options.height = (int) size.y;
        options.initialPosition = screenPosition;

        // Nothing is painted behind the content, so the panel's own fill is
        // the whole window: its alpha reaches the screen and the rounding is
        // the content's to cut — which is what cornerRadius would otherwise
        // have asked the window for.
        options.transparentBackground = true;

        popup = std::make_unique<Window>(menu, options);

        // Destroyed rather than hidden, which is the lifetime a menu wants:
        // nothing of it survives the click that closed it, and the next one is
        // built from whatever the app looks like then.
        popup->events.onDismissRequested = [this] { closeMenu(); };
    }

    // Deferred, because an item's click is dispatched from inside the popup's
    // own window: taking that window down on the way back out of its event is
    // not something to ask AppKit to survive. The generation is for the menu
    // reopened before the drop lands — that one is not the one being closed.
    void closeMenu()
    {
        auto generation = openCount;

        auto drop = [this, generation]
        {
            if (openCount == generation)
                popup.reset();
        };

        Threads::callAsync(drop);
    }

    Toolbar toolbar;
    NativeChildSurface pluginArea;
    MenuContent menu;
    std::unique_ptr<Window> popup;
    int openCount = 0;
    bool foreignContentAttached = false;
};

WindowOptions demoWindowOptions()
{
    auto options = WindowOptions {};
    options.title = "Popup over a foreign native view";
    options.width = 720;
    options.height = 460;
    options.minWidth = 520;
    options.minHeight = 320;

    return options;
}
} // namespace

int main()
{
    return runWindowedApp<PopupDemo>(demoWindowOptions());
}
