#include <eacp/Core/Threads/Timer.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/WebView/WebView.h>

#include <cmath>
#include <functional>
#include <string>

using namespace eacp;
using namespace eacp::Graphics;

// ---------------------------------------------------------------------------
// 1. A GPU surface: a Metal/D3D triangle that spins, built with the shader EDSL.
// ---------------------------------------------------------------------------

struct GpuVec2
{
    float x, y;
};

struct GpuRGB
{
    float r, g, b;
};

struct GpuVertex
{
    GpuVec2 position;
    GpuRGB color;
};

EACP_SHADER_VALUE(GpuVec2, Float2)
EACP_SHADER_VALUE(GpuRGB, Float3)

namespace
{
using namespace eacp::GPU;

const GpuVertex spinningTriangle[] = {
    {{0.0f, 0.8f}, {1.0f, 0.3f, 0.3f}},
    {{-0.8f, -0.8f}, {0.3f, 1.0f, 0.4f}},
    {{0.8f, -0.8f}, {0.35f, 0.5f, 1.0f}},
};

struct SpinShader final : ShaderProgram
{
    Uniform<Float> angle;

    EACP_SHADER(angle)

    SpinShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&GpuVertex::position);
        auto color = vertexInput(&GpuVertex::color);
        auto varyingColor = varying(color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, px * s + py * c);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(float4(varyingColor, 1.0f));
    }
};
} // namespace

struct GpuSurface final : GPU::GPUView
{
    GpuSurface()
    {
        setHandlesMouseEvents(true);
        shader.setVertices(spinningTriangle);
        shader.prepare(sampleCount());
    }

    void mouseDown(const MouseEvent&) override { spin = -spin * 1.5f; }

    void advance()
    {
        angle += spin;
        repaint();
    }

    void render(GPU::Frame& frame) override
    {
        shader.angle = angle;

        auto pass = frame.beginPass({Color {0.05f, 0.06f, 0.09f}});
        pass.draw(shader);
    }

    SpinShader shader;
    float angle = 0.0f;
    float spin = 0.02f;
    Threads::Timer timer {[this] { advance(); }, 60};
};

// ---------------------------------------------------------------------------
// 2. A plain View drawn entirely with immediate-mode Core Graphics primitives.
// ---------------------------------------------------------------------------

struct PrimitiveSurface final : View
{
    struct Ripple
    {
        Point center;
        float radius = 0.0f;
    };

    PrimitiveSurface() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent& event) override
    {
        ripples.add(Ripple {event.pos, 0.0f});
    }

    void advance()
    {
        phase += 0.045f;

        auto living = Vector<Ripple> {};

        for (auto& ripple: ripples)
        {
            ripple.radius += 2.6f;

            if (ripple.radius < maxRippleRadius)
                living.add(ripple);
        }

        ripples = living;
        repaint();
    }

    void paint(Context& g) override
    {
        auto bounds = getLocalBounds();

        // A translucent wash rather than a solid fill, so panels stacked behind
        // this one show through and blend with the primitives drawn on top.
        g.setColor(Color {0.07f, 0.08f, 0.11f, 0.55f});
        g.fillRect(bounds);

        paintGrid(g, bounds);
        paintWave(g, bounds);
        paintOrbiters(g, bounds);
        paintRipples(g, bounds);

        g.setColor(Color {0.6f, 0.85f, 1.0f});
        g.drawText("Immediate-mode primitives", {14.0f, 24.0f}, labelFont);

        g.setColor(Color::white(0.45f));
        g.drawText("lines · paths · fills · text — click to ripple",
                   {14.0f, bounds.h - 16.0f},
                   hintFont);
    }

    void paintGrid(Context& g, const Rect& bounds) const
    {
        g.setLineWidth(1.0f);
        g.setColor(Color::white(0.06f));

        auto spacing = 34.0f;

        for (auto column = 0; (float) column * spacing < bounds.w; ++column)
        {
            auto x = (float) column * spacing;
            g.drawLine({x, 0.0f}, {x, bounds.h});
        }

        for (auto row = 0; (float) row * spacing < bounds.h; ++row)
        {
            auto y = (float) row * spacing;
            g.drawLine({0.0f, y}, {bounds.w, y});
        }
    }

    void paintWave(Context& g, const Rect& bounds) const
    {
        auto wave = Path();
        auto midY = bounds.h * 0.5f;

        wave.moveTo({0.0f, midY});

        for (auto step = 0; (float) step * 6.0f <= bounds.w; ++step)
        {
            auto x = (float) step * 6.0f;
            auto y = midY + std::sin(x * 0.03f + phase) * bounds.h * 0.18f;
            wave.lineTo({x, y});
        }

        g.setLineWidth(2.0f);
        g.setColor(Color {0.4f, 0.8f, 1.0f, 0.8f});
        g.strokePath(wave);
    }

    void paintOrbiters(Context& g, const Rect& bounds) const
    {
        auto center = bounds.center();

        for (auto i = 0; i < 5; ++i)
        {
            auto a = phase * (1.0f + (float) i * 0.2f) + (float) i * 1.25f;
            auto radius = 30.0f + (float) i * 16.0f;
            auto p = Point {center.x + std::cos(a) * radius,
                            center.y + std::sin(a) * radius};

            auto dot = Path();
            dot.addEllipse({p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f});

            g.setColor(
                Color {1.0f - (float) i * 0.12f, 0.5f, 0.3f + (float) i * 0.1f});
            g.fillPath(dot);
        }
    }

    void paintRipples(Context& g, const Rect&) const
    {
        g.setLineWidth(2.0f);

        for (auto& ripple: ripples)
        {
            auto alpha = 1.0f - ripple.radius / maxRippleRadius;
            auto circle = Path();
            circle.addEllipse({ripple.center.x - ripple.radius,
                               ripple.center.y - ripple.radius,
                               ripple.radius * 2.0f,
                               ripple.radius * 2.0f});

            g.setColor(Color {0.5f, 1.0f, 0.7f, alpha});
            g.strokePath(circle);
        }
    }

    static constexpr float maxRippleRadius = 90.0f;

    float phase = 0.0f;
    Vector<Ripple> ripples;
    Font labelFont {FontOptions().withName("Helvetica-Bold").withSize(14.0f)};
    Font hintFont {FontOptions().withName("Helvetica").withSize(11.0f)};
    Threads::Timer timer {[this] { advance(); }, 60};
};

// ---------------------------------------------------------------------------
// 3. A draggable window-like Panel that frames any surface with primitive chrome
//    (a shape-layer border and a text-layer title bar) and routes mouse events.
// ---------------------------------------------------------------------------

struct Panel : View
{
    Panel(const std::string& titleToUse, const Color& accentToUse)
        : accent(accentToUse)
    {
        setHandlesMouseEvents(true);

        // Group opacity makes the whole panel — chrome plus its GPU / web /
        // primitive content — blend over the panels stacked behind it.
        setOpacity(panelOpacity);

        frame->setFillColor(Color {0.12f, 0.13f, 0.17f, 0.7f});
        frame->setStrokeColor(Color::white(0.22f));
        frame->setStrokeWidth(1.5f);

        titleBar->setFillColor(accent.withAlpha(0.92f));

        titleLabel->setText(titleToUse);
        titleLabel->setColor(Color::white());
        titleLabel->setFont(
            FontOptions().withName("Helvetica-Bold").withSize(13.0f));

        addChildren({frame, titleBar, titleLabel});
    }

    virtual View& getContent() = 0;
    virtual void layoutOverlay(const Rect&) {}

    void attachContent() { addSubview(getContent()); }

    // Grabbing the title bar starts a drag; releasing raises the panel so it is
    // never reordered mid-drag (which would drop the mouse capture).
    void mouseDown(const MouseEvent&) override
    {
        auto* host = getParent();

        if (host == nullptr)
            return;

        auto mouse = host->getMousePosition();
        auto bounds = getBounds();
        dragOffset = {mouse.x - bounds.x, mouse.y - bounds.y};
    }

    void mouseDragged(const MouseEvent&) override
    {
        auto* host = getParent();

        if (host == nullptr)
            return;

        auto mouse = host->getMousePosition();
        auto bounds = getBounds();
        setBounds(
            bounds.withPosition(mouse.x - dragOffset.x, mouse.y - dragOffset.y));
    }

    void mouseUp(const MouseEvent&) override
    {
        if (onRaise)
            onRaise();
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        auto border = Path();
        border.addRoundedRect(bounds.inset(1.0f), 10.0f);
        frame->setPath(border);
        frame.scaleToFit();

        auto strip = bounds.withHeight(titleHeight);
        titleBar.setBounds(strip);

        auto stripPath = Path();
        stripPath.addRoundedRect({0.0f, 0.0f, strip.w, strip.h}, 9.0f);
        titleBar->setPath(stripPath);

        titleLabel.setBounds(strip.inset(12.0f, 6.0f));

        auto content =
            Rect {3.0f, titleHeight, bounds.w - 6.0f, bounds.h - titleHeight - 3.0f};
        getContent().setBounds(content);

        layoutOverlay(content);
    }

    std::function<void()> onRaise;

protected:
    static constexpr float titleHeight = 28.0f;
    static constexpr float panelOpacity = 0.85f;
    Color accent;

private:
    ShapeLayerView frame;
    ShapeLayerView titleBar;
    TextLayerView titleLabel;
    Point dragOffset;
};

struct PrimitivePanel final : Panel
{
    PrimitivePanel()
        : Panel("Primitives · Core Graphics", {0.35f, 0.75f, 0.45f})
    {
        attachContent();
    }

    View& getContent() override { return surface; }

    PrimitiveSurface surface;
};

struct GpuPanel final : Panel
{
    GpuPanel()
        : Panel("GPU · spinning triangle", {0.85f, 0.35f, 0.55f})
    {
        attachContent();

        badge->setFillColor(Color::black(0.45f));
        badgeLabel->setText("a primitive layer, composited over GPU pixels");
        badgeLabel->setColor(Color {1.0f, 0.9f, 0.55f});
        badgeLabel->setFont(FontOptions().withName("Helvetica").withSize(11.0f));

        addChildren({badge, badgeLabel});
    }

    View& getContent() override { return surface; }

    // A shape + text layer parked on top of the Metal surface proves primitives
    // can be layered above native GPU content within one panel.
    void layoutOverlay(const Rect& content) override
    {
        auto box = Rect {content.x + 10.0f,
                         content.y + content.h - 30.0f,
                         content.w - 20.0f,
                         22.0f};
        badge.setBounds(box);

        auto rounded = Path();
        rounded.addRoundedRect({0.0f, 0.0f, box.w, box.h}, 6.0f);
        badge->setPath(rounded);

        badgeLabel.setBounds(box.inset(8.0f, 3.0f));
    }

    GpuSurface surface;
    ShapeLayerView badge;
    TextLayerView badgeLabel;
};

struct WebPanel final : Panel
{
    WebPanel()
        : Panel("WebView · WKWebView", {0.3f, 0.6f, 0.9f})
    {
        surface.loadHTML(pageHtml());
        attachContent();
    }

    View& getContent() override { return surface; }

    static std::string pageHtml()
    {
        return R"HTML(
<!doctype html><html><head><meta charset="utf-8"><style>
  :root { color-scheme: dark; }
  html,body { margin:0; height:100%; font-family:-apple-system,system-ui,sans-serif; }
  body { background:radial-gradient(120% 120% at 30% 20%,#16324f,#0a1622);
         color:#eaf2ff; display:flex; flex-direction:column; gap:14px;
         align-items:center; justify-content:center; user-select:none; }
  h1 { margin:0; font-size:17px; font-weight:600; }
  p  { margin:0; font-size:12px; color:#9fb6d6; }
  button { border:1px solid #3a5f8a; background:#16365c; color:#eaf2ff;
           padding:9px 16px; border-radius:9px; font-size:13px; cursor:pointer; }
  button:hover { background:#1d4775; }
  #swatch { width:120px; height:46px; border-radius:9px; cursor:pointer;
            background:#4f8fd6; transition:background .15s; }
  #count { font-variant-numeric:tabular-nums; color:#7fe7b0; }
</style></head><body>
  <h1>Live web content</h1>
  <p>This panel's mouse events are handled by the web engine.</p>
  <button id="ping">clicked <span id="count">0</span>&times;</button>
  <div id="swatch" title="click to recolor"></div>
  <script>
    var n = 0;
    var hue = 210;
    document.getElementById('ping').addEventListener('click', function () {
      document.getElementById('count').textContent = ++n;
    });
    document.getElementById('swatch').addEventListener('click', function () {
      hue = (hue + 47) % 360;
      this.style.background = 'hsl(' + hue + ' 60% 58%)';
    });
  </script>
</body></html>)HTML";
    }

    WebView surface;
};

// ---------------------------------------------------------------------------
// 4. The root view: an animated primitive backdrop with the three panels layered
//    on top, each raisable to the front.
// ---------------------------------------------------------------------------

struct LayeredRoot final : View
{
    LayeredRoot()
    {
        addChildren({primitivePanel, gpuPanel, webPanel});

        wireRaise(primitivePanel);
        wireRaise(gpuPanel);
        wireRaise(webPanel);
    }

    void wireRaise(Panel& panel)
    {
        panel.onRaise = [this, &panel] { bringToFront(panel); };
    }

    void bringToFront(View& view)
    {
        auto& children = getSubviews();

        if (!children.empty() && children.back() == &view)
            return;

        removeSubview(view);
        addSubview(view);
    }

    void advance()
    {
        drift += 0.6f;
        repaint();
    }

    void paint(Context& g) override
    {
        auto bounds = getLocalBounds();

        g.setColor(Color {0.04f, 0.045f, 0.06f});
        g.fillRect(bounds);

        g.setLineWidth(1.0f);
        g.setColor(Color::white(0.04f));

        auto spacing = 46.0f;
        auto start = -bounds.h + std::fmod(drift, spacing);

        for (auto i = 0; start + (float) i * spacing < bounds.w; ++i)
        {
            auto x = start + (float) i * spacing;
            g.drawLine({x, 0.0f}, {x + bounds.h, bounds.h});
        }

        g.setColor(Color {0.55f, 0.65f, 0.85f, 0.9f});
        g.drawText("eacp · translucent layered surfaces — drag to overlap "
                   "and blend, interact inside each panel",
                   {22.0f, 26.0f},
                   titleFont);
    }

    void resized() override
    {
        primitivePanel.setBounds({60.0f, 70.0f, 380.0f, 300.0f});
        gpuPanel.setBounds({300.0f, 200.0f, 360.0f, 300.0f});
        webPanel.setBounds({200.0f, 360.0f, 440.0f, 320.0f});
        repaint();
    }

    PrimitivePanel primitivePanel;
    GpuPanel gpuPanel;
    WebPanel webPanel;
    float drift = 0.0f;
    Font titleFont {FontOptions().withName("Helvetica-Bold").withSize(13.0f)};
    Threads::Timer timer {[this] { advance(); }, 30};
};

struct MyApp
{
    MyApp() { window.setContentView(root); }

    static WindowOptions options()
    {
        auto opts = WindowOptions {};
        opts.width = 1100;
        opts.height = 760;
        opts.minWidth = 720;
        opts.minHeight = 520;
        opts.title = "Layered Views — Primitives · GPU · WebView";
        opts.backgroundColor = Color {0.04f, 0.045f, 0.06f};
        return opts;
    }

    LayeredRoot root;
    Window window {options()};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
