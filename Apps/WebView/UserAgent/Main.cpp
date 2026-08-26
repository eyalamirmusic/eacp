#include <eacp/WebView/WebView.h>

#include <array>

using namespace eacp;
using namespace Graphics;

// Two WebView options a container app — one window holding several
// third-party sites — cannot do without, shown together because one page
// tests both:
//
//   Options::userAgent                 what navigator.userAgent reports, for
//                                      the sites that refuse a runtime they
//                                      do not recognise as a desktop browser
//   Options::loadDeclinedPopupsInline  what happens to THIS page when the app
//                                      declines a target="_blank" request
//
// Pick a UA preset and the view is rebuilt with it — options are read at
// construction, so changing one means a new WebView. Then click the link in
// the page: with inline loading on, the page is replaced by example.com,
// which is what the framework has always done and what a container must not
// do. Turn it off and the page stays put; only the app's own line moves.

namespace
{
struct Preset
{
    const char* name;
    const char* userAgent;
};

constexpr auto presets = std::array {
    Preset {"Platform default", ""},
    Preset {"Safari 17 (macOS)",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
            "(KHTML, like Gecko) Version/17.4 Safari/605.1.15"},
    Preset {"Chrome 124 (Windows)",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
            "like Gecko) Chrome/124.0.0.0 Safari/537.36"}};

std::string demoPage()
{
    return R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  body { font: 15px/1.6 -apple-system, "Segoe UI", sans-serif; margin: 0;
         padding: 30px; background: #14161a; color: #e6e8ec; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .1em;
       color: #8b93a1; margin: 0 0 10px; }
  pre { background: #1d2027; border: 1px solid #2b303a; border-radius: 8px;
        padding: 14px; margin: 0 0 30px; white-space: pre-wrap;
        word-break: break-all; }
  a { color: #6ea8fe; }
</style></head>
<body>
  <h2>navigator.userAgent</h2>
  <pre id="ua"></pre>
  <h2>new window request</h2>
  <p><a href="https://example.com" target="_blank">
     Open example.com in a new window</a></p>
  <script>
    document.getElementById("ua").textContent = navigator.userAgent;
  </script>
</body></html>)HTML";
}
} // namespace

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

    void setSelected(bool shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }

    void paint(Context& g) override
    {
        auto fill = selected  ? Color {0.24f, 0.42f, 0.70f, 1.f}
                    : hovered ? Color::white(0.16f)
                              : Color::white(0.10f);

        g.setColor(fill);
        g.fillRoundedRect(getLocalBounds(), 7.f);

        g.setColor(Color::white(selected ? 0.98f : 0.72f));
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
    bool selected = false;
};

struct DemoRoot final : View
{
    DemoRoot()
    {
        for (auto i = 0; i < (int) presets.size(); ++i)
        {
            presetButtons[i].setText(presets[i].name);
            presetButtons[i].onClick = [this, i] { selectPreset(i); };
            addSubview(presetButtons[i]);
        }

        inlineButton.onClick = [this] { toggleInlineLoading(); };
        addSubview(inlineButton);

        updateButtons();
        rebuildWebView();
    }

    void resized() override
    {
        auto x = 14.f;

        for (auto& button: presetButtons)
        {
            button.setBounds({x, 12.f, 152.f, 30.f});
            x += 160.f;
        }

        inlineButton.setBounds({x + 12.f, 12.f, 210.f, 30.f});

        if (webView)
            webView->setBounds(webArea());
    }

    void paint(Context& g) override
    {
        g.setColor(Color {0.10f, 0.11f, 0.14f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor(Color::white(0.5f));
        g.drawText(statusText(), {16.f, webArea().y - 12.f}, statusFont);
    }

private:
    void selectPreset(int index)
    {
        presetIndex = index;
        updateButtons();
        rebuildWebView();
    }

    void toggleInlineLoading()
    {
        loadDeclinedPopupsInline = !loadDeclinedPopupsInline;
        updateButtons();
        rebuildWebView();
    }

    void updateButtons()
    {
        for (auto i = 0; i < (int) presets.size(); ++i)
            presetButtons[i].setSelected(i == presetIndex);

        inlineButton.setText(loadDeclinedPopupsInline ? "Inline load: ON"
                                                      : "Inline load: OFF");
        inlineButton.setSelected(loadDeclinedPopupsInline);
    }

    void rebuildWebView()
    {
        webView.reset();
        declinedURL.clear();

        auto options = WebView::Options();
        options.statusBar = false;
        options.userAgent = presets[presetIndex].userAgent;
        options.loadDeclinedPopupsInline = loadDeclinedPopupsInline;

        webView.emplace(options);
        addSubview(*webView);

        webView->onNewWindowRequested =
            [this](OwningPointer<WebView>, const std::string& url)
        {
            declinedURL = url;
            repaint();
            return false;
        };

        webView->loadHTML(demoPage());

        resized();
        repaint();
    }

    Rect webArea() const
    {
        auto bounds = getLocalBounds();
        return {14.f, 74.f, bounds.w - 28.f, bounds.h - 88.f};
    }

    std::string statusText() const
    {
        if (!declinedURL.empty())
            return "Declined a new window for " + declinedURL + " — the page "
                   + (loadDeclinedPopupsInline ? "was replaced by it"
                                               : "stayed where it was");

        return "Click the link in the page to see what a declined new-window "
               "request does to it";
    }

    Button presetButtons[presets.size()];
    Button inlineButton;
    std::optional<WebView> webView;
    std::string declinedURL;
    int presetIndex = 0;
    bool loadDeclinedPopupsInline = true;
    Font statusFont {FontOptions().withName("Helvetica").withSize(11.f)};
};

struct UserAgentApp
{
    UserAgentApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar(), window);
        window.setContentView(root);
    }

    static WindowOptions getOptions()
    {
        auto options = WindowOptions();

        options.title = "WebView User Agent";
        options.width = 900;
        options.height = 680;
        options.minWidth = 720;
        options.minHeight = 420;
        options.backgroundColor = Color {0.10f, 0.11f, 0.14f, 1.f};

        return options;
    }

    DemoRoot root;
    Window window {getOptions()};
};

int main()
{
    return eacp::Apps::run<UserAgentApp>();
}
