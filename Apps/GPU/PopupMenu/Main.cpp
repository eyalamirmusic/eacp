#include "MenuText.h"
#include "PanelShader.h"

#include <eacp/Graphics/Graphics.h>

#if EACP_HAS_CONTEXT
#include "../../Graphics/PopupWindow/ForeignView.h"
#else
#include "../../Plugins/SpinningTriangle.h"
#endif

#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

// Apps/Graphics/PopupWindow's scenario with the popup drawn on the GPU: the
// menu a plugin host has to be able to put over a plugin's editor, except that
// every pixel of it — panel, drop shadow, hover highlight and the band
// travelling across it — comes out of one fragment shader and one instanced
// draw, with the labels from eacp-text's glyph atlas over the top.
//
// Which is the interesting part rather than a flourish. A popup is a window of
// its own, so its content is composited by the system rather than painted into
// the window under it, and a shadow that fades into whatever is behind it needs
// the window itself to carry alpha. That works on macOS and Windows and not on
// Linux — see popupCarriesAlpha below — so the example asks and degrades.
//
// What is worth watching while it runs:
//   - the menu draws over the foreign view, which shows through the panel's
//     own body as well as through the shadow fading into it;
//   - it scales and fades open in about an eighth of a second;
//   - the item under the pointer eases into the accent colour and a highlight
//     band travels across it, although the popup is never key;
//   - the main window's title bar stays active the whole time;
//   - a click outside dismisses the menu AND the plugin's button does not
//     fire, because the press that closed the menu belonged to the menu;
//   - Escape dismisses it, and dragging the main window carries it along;
//   - every open builds a different list, so the popup is a different size
//     each time — there is no cached window being shown again;
//   - nothing renders at all once the open settles and the pointer leaves.

using namespace eacp;
using namespace Graphics;
using namespace PopupMenu;

namespace
{
// Whether a popup Window's own pixels can be translucent, which is what lets a
// shadow fade into the window underneath instead of ending at a rectangle.
//
// macOS composites the CAMetalLayer with alpha once the layer is told not to be
// opaque, and Windows gives a transparentBackground window a
// DXGI_ALPHA_MODE_PREMULTIPLIED swapchain with no redirection bitmap. Linux has
// neither yet: the Vulkan swapchain asks for VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
// whatever the window said, and X11 windows are made on the screen's 24-bit
// root visual, which has no alpha channel to begin with. So there the popup is
// opaque and the margin the shadow lives in is filled with the app's own dark
// background — a shadow on a dark surface rather than on the desktop.
#if defined(__linux__)
constexpr auto popupCarriesAlpha = false;
#else
constexpr auto popupCarriesAlpha = true;
#endif

// The panel's own body is see-through where the window carries alpha, so the
// foreign view under the menu shows through it and not only around it. Where
// it does not, the same fill would only darken against the app's background,
// so there it stays as opaque as it was.
constexpr auto panelOpacity = popupCarriesAlpha ? 0.80f : 0.97f;

// Higher than the body, so a hovered item reads as a solid bar over a
// see-through panel rather than as one more layer of it.
constexpr auto highlightOpacity = 0.95f;

constexpr auto background = Color {0.07f, 0.08f, 0.10f};
constexpr auto toolbarColor = Color {0.15f, 0.16f, 0.19f};
constexpr auto panelColor = Color {0.14f, 0.15f, 0.18f, panelOpacity};
constexpr auto panelEdgeColor = Color {0.34f, 0.36f, 0.42f, 0.92f};
constexpr auto rowColor = Color {0.24f, 0.26f, 0.31f};
constexpr auto accentColor = Color {0.22f, 0.46f, 0.92f, 0.88f};
constexpr auto labelColor = Color::white();
constexpr auto hintColor = Color::white(0.45f);
constexpr auto shadowColor = Color::black(0.55f);

constexpr auto toolbarHeight = 56.f;
constexpr auto itemHeight = 30.f;
constexpr auto menuPadding = 8.f;
constexpr auto menuCorner = 11.f;
constexpr auto itemCorner = 6.f;
constexpr auto textInset = 12.f;
constexpr auto minMenuWidth = 180.f;

constexpr auto shadowBlur = 16.f;
constexpr auto shadowDrop = 7.f;

// The transparent gutter the panel sits inside, wide enough for the whole
// blurred falloff to land in. It is part of the popup window's size, so the
// window is opened that much up and to the left of where the panel goes.
constexpr auto shadowMargin = shadowBlur + shadowDrop + 2.f;

constexpr auto openSeconds = 0.12f;
constexpr auto hoverSeconds = 0.13f;
constexpr auto sheenTurnsPerSecond = 0.45f;

float easeOutCubic(float amount)
{
    const auto remaining = 1.f - amount;

    return 1.f - remaining * remaining * remaining;
}

// Exponential approach rather than a linear ramp: a highlight that eases in and
// out reads as a menu, and one that flips reads as a repaint.
float approach(float value, float target, float delta, float seconds)
{
    const auto rate = 1.f - std::exp(-delta / seconds);

    return value + (target - value) * rate;
}

Rect scaledAbout(const Rect& rect, Point pivot, float scale)
{
    return {pivot.x + (rect.x - pivot.x) * scale,
            pivot.y + (rect.y - pivot.y) * scale,
            rect.w * scale,
            rect.h * scale};
}

std::string currentTimeText()
{
    auto now = std::time(nullptr);
    char text[16] {};
    std::strftime(text, sizeof(text), "%H:%M:%S", std::localtime(&now));

    return text;
}

// Built again on every open, and deliberately a different length each time: a
// popup whose size is decided by its content is the case a cached window
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

struct MenuRow
{
    std::string text;
    float hover = 0.f;
};

// The popup's content: one GPUView for the whole menu rather than a view per
// item, because every item is an instance of one shader and a view apiece would
// be a swapchain apiece.
struct MenuView final : GPU::GPUView
{
    MenuView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);
        setMouseCursor(MouseCursor::PointingHand);
    }

    // The scale comes from the view the menu was opened over: this one is in no
    // window yet, and the labels have to be measurable before there is a window
    // to size.
    void setItems(const Vector<std::string>& texts, float scale)
    {
        text.setScale(scale);
        rows.clear();

        for (const auto& item: texts)
            rows.add({item, 0.f});

        hovered = -1;
        openAmount = 0.f;
        sheenPhase = 0.f;

        setContinuous(true);
    }

    // Panel plus the gutter its shadow needs, which is the popup window's size.
    Point preferredSize()
    {
        auto width = minMenuWidth;

        for (const auto& row: rows)
            width = std::max(width, text.measure(row.text) + textInset * 2.f);

        const auto height = menuPadding * 2.f + itemHeight * (float) rows.size();

        return {std::ceil(width) + shadowMargin * 2.f, height + shadowMargin * 2.f};
    }

    void update(Threads::FrameTime frame) override
    {
        const auto delta = (float) frame.delta;

        openAmount = std::min(1.f, openAmount + delta / openSeconds);

        for (auto index = 0; index < rows.size(); ++index)
        {
            auto& row = rows[index];
            row.hover = approach(
                row.hover, index == hovered ? 1.f : 0.f, delta, hoverSeconds);
        }

        if (hovered >= 0)
            sheenPhase += delta * sheenTurnsPerSecond;

        // Idle the moment nothing is moving: a menu sitting open under no
        // pointer should cost the GPU exactly nothing. repaint() rather than
        // relying on this tick, so the settled frame is drawn either way.
        if (!isAnimating())
        {
            setContinuous(false);
            repaint();
        }
    }

    void render(GPU::Frame& frame) override
    {
        text.setScale(backingScale());

        const auto size = frame.logicalSize();

        // Transparent where the window carries alpha, so the shadow fades into
        // whatever is behind the popup; the app's own background where it does
        // not, which is the best an opaque window can do.
        auto pass = frame.beginPass(
            {popupCarriesAlpha ? Color {0.f, 0.f, 0.f, 0.f} : background});

        panels.begin(size, backingScale());
        text.begin(size);

        const auto opening = easeOutCubic(openAmount);
        const auto scale = 0.92f + 0.08f * opening;
        const auto fade = opening;

        const auto panel = panelRect(size);
        const auto pivot = panel.center();
        const auto grown = scaledAbout(panel, pivot, scale);

        panels.fillShadow(grown,
                          shadowColor.withAlpha(shadowColor.a * fade),
                          menuCorner * scale,
                          shadowBlur,
                          {0.f, shadowDrop * scale});

        // The panel is the lighter rect with the darker one a point inside it,
        // which is a hairline border for one extra instance — and what stops a
        // dark panel on a dark background reading as a hole.
        panels.fillRect(grown,
                        panelEdgeColor.withAlpha(panelEdgeColor.a * fade),
                        menuCorner * scale);
        panels.fillRect(grown.inset(1.f),
                        panelColor.withAlpha(panelColor.a * fade),
                        (menuCorner - 1.f) * scale);

        const auto baselineOffset = (text.ascent() - text.descent()) * 0.5f;

        for (auto index = 0; index < rows.size(); ++index)
        {
            const auto& row = rows[index];
            const auto item = scaledAbout(itemRect(panel, index), pivot, scale);

            if (row.hover > 0.003f)
            {
                panels.fillHighlight(
                    item,
                    rowColor.withAlpha(highlightOpacity * row.hover * fade),
                    accentColor,
                    itemCorner * scale,
                    row.hover,
                    sheenPhase);
            }

            const auto pen =
                Point {item.x + textInset * scale, item.center().y + baselineOffset};

            text.add(row.text, pen, labelColor.withAlpha(labelColor.a * fade));
        }

        panels.flush(pass);
        text.flush(pass);
    }

    void mouseMoved(const MouseEvent& event) override { setHovered(event.pos); }
    void mouseDragged(const MouseEvent& event) override { setHovered(event.pos); }

    void mouseExited(const MouseEvent&) override { setHovered({-1.f, -1.f}); }

    void mouseUp(const MouseEvent& event) override
    {
        const auto index = itemAt(event.pos);

        if (index >= 0)
            onChosen(rows[index].text);
    }

    std::function<void(const std::string&)> onChosen = [](auto&&) {};

private:
    bool isAnimating() const
    {
        if (openAmount < 1.f || hovered >= 0)
            return true;

        for (const auto& row: rows)
            if (row.hover > 0.003f)
                return true;

        return false;
    }

    Rect panelRect(Point size) const
    {
        return Rect {0.f, 0.f, size.x, size.y}.inset(shadowMargin);
    }

    Rect itemRect(const Rect& panel, int index) const
    {
        return {panel.x + menuPadding,
                panel.y + menuPadding + itemHeight * (float) index,
                panel.w - menuPadding * 2.f,
                itemHeight};
    }

    int itemAt(Point position)
    {
        const auto bounds = getLocalBounds();
        const auto panel = panelRect({bounds.w, bounds.h});

        for (auto index = 0; index < rows.size(); ++index)
            if (itemRect(panel, index).contains(position))
                return index;

        return -1;
    }

    void setHovered(Point position)
    {
        const auto index = itemAt(position);

        if (index == hovered)
            return;

        hovered = index;
        setContinuous(true);
    }

    PanelBatch panels;
    MenuText text;
    Vector<MenuRow> rows;

    int hovered = -1;
    float openAmount = 0.f;
    float sheenPhase = 0.f;
};

// The host window's own content: GPU-drawn too, so the Menu button hovers with
// the same shader the menu items do.
struct ToolbarView final : GPU::GPUView
{
    ToolbarView()
    {
        setSampleCount(1);
        setHandlesMouseEvents(true);
    }

    void update(Threads::FrameTime frame) override
    {
        const auto delta = (float) frame.delta;

        buttonHover =
            approach(buttonHover, pointerOnButton ? 1.f : 0.f, delta, hoverSeconds);

        if (pointerOnButton)
            sheenPhase += delta * sheenTurnsPerSecond;
        else if (buttonHover <= 0.003f)
        {
            setContinuous(false);
            repaint();
        }
    }

    void render(GPU::Frame& frame) override
    {
        text.setScale(backingScale());

        const auto size = frame.logicalSize();
        auto pass = frame.beginPass({toolbarColor});

        panels.begin(size, backingScale());
        text.begin(size);

        const auto button = buttonRect();

        panels.fillHighlight(button,
                             rowColor.withAlpha(0.25f + 0.70f * buttonHover),
                             accentColor,
                             7.f,
                             buttonHover,
                             sheenPhase);

        const auto baseline =
            button.center().y + (text.ascent() - text.descent()) * 0.5f;

        text.add("Menu", {button.x + textInset, baseline}, labelColor);

        text.add("Right-click anywhere in this strip, or press Menu",
                 {button.right() + 20.f, baseline},
                 hintColor);

        panels.flush(pass);
        text.flush(pass);
    }

    void mouseMoved(const MouseEvent& event) override
    {
        const auto inside = buttonRect().contains(event.pos);

        if (inside == pointerOnButton)
            return;

        pointerOnButton = inside;
        setMouseCursor(inside ? MouseCursor::PointingHand : MouseCursor::Default);
        setContinuous(true);
    }

    void mouseExited(const MouseEvent&) override
    {
        pointerOnButton = false;
        setContinuous(true);
    }

    // Right anywhere, which is what a mixer strip does: the menu opens under
    // the pointer rather than under a control.
    void mouseDown(const MouseEvent& event) override
    {
        if (event.button == MouseButton::Right)
        {
            onMenuRequested(localToScreen(event.pos));
            return;
        }

        if (buttonRect().contains(event.pos))
        {
            const auto below = Point {buttonRect().x, buttonRect().bottom() + 6.f};
            onMenuRequested(localToScreen(below));
        }
    }

    std::function<void(Point)> onMenuRequested = [](auto&&) {};

private:
    Rect buttonRect() const
    {
        return getLocalBounds().inset(14.f, 13.f).fromLeft(96.f);
    }

    PanelBatch panels;
    MenuText text;

    bool pointerOnButton = false;
    float buttonHover = 0.f;
    float sheenPhase = 0.f;
};

struct PopupMenuDemo final : View
{
    PopupMenuDemo()
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

    void resized() override
    {
        auto area = getLocalBounds();

        toolbar.setBounds(area.removeFromTop(toolbarHeight));
        pluginArea.setBounds(area);

        attachForeignContent();
    }

private:
#if EACP_HAS_CONTEXT
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
#else
    // The stand-in below is an eacp view like any other and needs no attaching.
    void attachForeignContent() {}
#endif

    void openMenu(Point screenPosition)
    {
        // The old one first: one menu at a time, and its content view is about
        // to be adopted by the new window.
        popup.reset();

        // The press that opened the menu belongs to the menu from here on, so
        // the toolbar stops waiting for an up that is never coming.
        cancelMouseCapture();

        ++openCount;
        menu.setItems(buildMenuItems(openCount), toolbar.backingScale());

        const auto size = menu.preferredSize();

        auto options = WindowOptions {};
        options.popup = true;
        options.parent = getWindow();
        options.title = "Menu";
        options.width = (int) std::ceil(size.x);
        options.height = (int) std::ceil(size.y);
        options.transparentBackground = popupCarriesAlpha;
        options.backgroundColor = background;

        // The window is opened around the panel rather than on it: the gutter
        // the shadow lives in is part of its size, so the panel's own top-left
        // is what lands where the pointer was.
        options.initialPosition =
            Point {screenPosition.x - shadowMargin, screenPosition.y - shadowMargin};

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

    ToolbarView toolbar;

#if EACP_HAS_CONTEXT
    NativeChildSurface pluginArea;
    bool foreignContentAttached = false;
#else
    // Linux has no NativeChildSurface, so this stands in for the hosted
    // plugin's editor: Apps/Plugins' own spinning triangle, driven by a timer of
    // its own, so what the popup covers is visibly alive rather than a still.
    PluginDemo::SpinningTriangleView pluginArea {{0.18f, 0.34f, 0.52f}};
    Threads::Timer spin {[this] { pluginArea.advance(); }, 60};
#endif

    MenuView menu;
    std::unique_ptr<Window> popup;
    int openCount = 0;
};

WindowOptions demoWindowOptions()
{
    auto options = WindowOptions {};

    options.title = "GPU popup over a foreign native view";
    options.width = 760;
    options.height = 480;
    options.minWidth = 520;
    options.minHeight = 320;
    options.backgroundColor = background;

    return options;
}
} // namespace

int main()
{
    return runWindowedApp<PopupMenuDemo>(demoWindowOptions());
}
