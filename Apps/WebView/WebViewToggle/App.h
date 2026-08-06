#pragma once

#include "Types.h"

#include <eacp/WebView/WebView.h>

#include <cstdlib>
#include <optional>

using namespace eacp;

// A push button drawn by the framework itself, so the window's chrome stays
// fully native while the web content comes and goes.
struct ToggleButton final : Graphics::View
{
    ToggleButton()
    {
        setHandlesMouseEvents(true);
        setMouseCursor(Graphics::MouseCursor::PointingHand);
    }

    void setText(std::string newText)
    {
        text = std::move(newText);
        repaint();
    }

    void paint(Graphics::Context& g) override
    {
        auto bounds = getLocalBounds();

        auto fill = pressed   ? Graphics::Color {0.24f, 0.42f, 0.70f, 1.0f}
                    : hovered ? Graphics::Color {0.40f, 0.66f, 0.98f, 1.0f}
                              : Graphics::Color {0.33f, 0.56f, 0.90f, 1.0f};

        g.setColor(fill);
        g.fillRoundedRect(bounds, 8.0f);

        g.setColor(Graphics::Color {1.0f, 1.0f, 1.0f, 0.95f});
        g.drawText(text, {14.0f, bounds.h * 0.5f + 4.5f}, font);
    }

    void mouseEntered(const Graphics::MouseEvent&) override
    {
        hovered = true;
        repaint();
    }

    void mouseExited(const Graphics::MouseEvent&) override
    {
        hovered = false;
        pressed = false;
        repaint();
    }

    void mouseDown(const Graphics::MouseEvent&) override
    {
        pressed = true;
        repaint();
    }

    void mouseUp(const Graphics::MouseEvent& event) override
    {
        pressed = false;
        repaint();

        if (getLocalBounds().contains(event.pos))
            onClick();
    }

    std::function<void()> onClick = [] {};

    std::string text = "Open WebView";
    Graphics::Font font {
        Graphics::FontOptions().withName("Helvetica-Bold").withSize(13.0f)};
    bool hovered = false;
    bool pressed = false;
};

// ephemeralSession ties the page's network session to the WebView, so on
// macOS the WebKit Networking helper process is destroyed with it instead
// of idling until app exit; the page keeps no storage across reopens,
// which this demo wants anyway.
inline Graphics::WebView::Options toggleWebViewOptions()
{
    auto options = Graphics::embeddedOptions("ToggleApp");
    options.ephemeralSession = true;
    return options;
}

// Everything whose lifetime is one open/close cycle, bundled so a single
// emplace()/reset() creates and destroys it as a unit. Declaration order is
// the destruction contract, in reverse: the timer dies first (native ticks
// stop), then the bridge (listeners and handlers torn down), then the
// WebView itself (the OS web runtime and its helper processes let go), and
// the api last, after everything that could still call into it.
struct WebPanel
{
    Api::PanelApi api;
    Graphics::WebView webView {toggleWebViewOptions()};
    Graphics::WebViewBridge transport {webView, api};
    Threads::Timer timer {[this] { api.advanceTick(); }, 30};
};

struct ToggleRoot final : Graphics::View
{
    ToggleRoot()
    {
        addSubview(button);
        button.onClick = [this] { toggle(); };
    }

    bool isOpen() const { return panel.has_value(); }

    void open()
    {
        if (panel)
            return;

        panel.emplace();
        panel->api.onPageClick = [this](long long) { repaint(); };
        addSubview(panel->webView);
        panel->webView.setBounds(webArea());

        openCount++;
        button.setText("Close WebView");
        repaint();
    }

    void close()
    {
        if (!panel)
            return;

        panel.reset();

        button.setText("Open WebView");
        repaint();
    }

    void toggle()
    {
        if (isOpen())
            close();
        else
            open();
    }

    void resized() override
    {
        button.setBounds({16.0f, 16.0f, 150.0f, 36.0f});

        if (panel)
            panel->webView.setBounds(webArea());
    }

    Graphics::Rect webArea() const
    {
        auto bounds = getLocalBounds();
        return {16.0f, 68.0f, bounds.w - 32.0f, bounds.h - 84.0f};
    }

    void paint(Graphics::Context& g) override
    {
        auto bounds = getLocalBounds();

        g.setColor(Graphics::Color {0.10f, 0.11f, 0.14f, 1.0f});
        g.fillRect(bounds);

        g.setColor(Graphics::Color::white(0.55f));
        g.drawText(statusText(), {182.0f, 38.5f}, statusFont);

        if (!panel)
        {
            g.setColor(Graphics::Color::white(0.06f));
            g.fillRoundedRect(webArea(), 10.0f);

            g.setColor(Graphics::Color::white(0.35f));
            g.drawText("No WebView. No bridge. No content or networking "
                       "processes.",
                       {webArea().x + 18.0f, webArea().y + 32.0f},
                       statusFont);
        }
    }

    std::string statusText() const
    {
        if (panel)
        {
            auto clicks = panel->api.getState().pageClicks;
            return "WebView open — pings from page: " + std::to_string(clicks);
        }

        if (openCount > 0)
        {
            return "WebView destroyed (opened " + std::to_string(openCount)
                   + "x this run)";
        }

        return "Click the button to create a WebView + bridge dynamically";
    }

    ToggleButton button;
    std::optional<WebPanel> panel;
    int openCount = 0;
    Graphics::Font statusFont {
        Graphics::FontOptions().withName("Helvetica").withSize(12.0f)};
};

inline Graphics::WindowOptions toggleWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 960;
    options.height = 660;
    options.title = "WebView Toggle";
    return options;
}

struct MyApp
{
    MyApp()
    {
        Graphics::setApplicationMenuBar(Graphics::buildDefaultWebViewMenuBar(),
                                        window);
        window.setContentView(root);
        setupDemoAutomation();
    }

    // EACP_DEMO_AUTOTOGGLE_SECONDS=N flips the panel every N seconds, and
    // EACP_DEMO_AUTOQUIT_SECONDS ends the run — together they make process
    // teardown observable from a script (watch the OS web helper processes
    // appear and disappear while the app cycles).
    void setupDemoAutomation()
    {
        auto toggleSeconds =
            std::atoi(getEnvValue("EACP_DEMO_AUTOTOGGLE_SECONDS").c_str());

        if (toggleSeconds > 0)
        {
            autoToggle.emplace(
                [this, toggleSeconds]
                {
                    if (++toggleTicks % toggleSeconds == 0)
                        root.toggle();
                },
                1);
        }

        auto quitSeconds =
            std::atoi(getEnvValue("EACP_DEMO_AUTOQUIT_SECONDS").c_str());

        if (quitSeconds > 0)
        {
            autoQuit.emplace(
                [this, quitSeconds]
                {
                    if (++elapsedSeconds >= quitSeconds)
                        Apps::quit();
                },
                1);
        }
    }

    ToggleRoot root;
    Graphics::Window window {toggleWindowOptions()};
    std::optional<Threads::Timer> autoToggle;
    std::optional<Threads::Timer> autoQuit;
    int toggleTicks = 0;
    int elapsedSeconds = 0;
};
