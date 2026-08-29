#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// A transform on a layer's composite: the one place in the component tier
// where a picture is not upright.
//
// Everything else here is drawn through a scissor rect, which cannot express a
// turned region -- so a rotation belongs to the one primitive that is already
// four corners of a texture rather than a rectangle to be cut. A layer is
// rendered once, standing still, and the matrix places the quad it is drawn as.
//
// Which is why turning one is a frame and not a pass: the footer counts the
// layers rendered on the last frame, and the card at the bottom spins the whole
// time on that count being zero. The texture is the content at full size, so a
// scaled layer magnifies its own pixels, exactly as a compositor does.
//
// The matrix is in the layer's *own* space, its top-left the origin, so a
// caller turning a box about its middle names half its size rather than where
// the box happens to sit -- and a layer inside another one composites the same
// wherever the outer one put it, which is the bottom-right pair.

using namespace eacp;

namespace
{
using UI::Color;
using UI::Point;
using UI::Rect;

constexpr auto cardWidth = 120.f;
constexpr auto cardHeight = 76.f;
constexpr auto margin = 30.f;

// The room round the card its turned corners need: a square turned on its
// point is as wide as its diagonal, and a layer is composited within the
// component that draws it.
constexpr auto room = 60.f;

GPUWidgets::AffineTransform about(const GPUWidgets::AffineTransform& transform,
                                  Point centre)
{
    return GPUWidgets::AffineTransform::translation(-centre.x, -centre.y)
        .then(transform)
        .then(GPUWidgets::AffineTransform::translation(centre.x, centre.y));
}

// A card with a stripe down it, so a turn is legible and an overlap is
// visible: the whole point of a layer is that the group is composited as one
// thing, and the stripe is what shows it.
void paintCard(UI::Graphics& g, const Rect& box, std::string_view caption)
{
    g.setColour(UI::defaultTheme().panel);
    g.fillRoundedRect(box, 10.f);

    g.setColour(Color {0.31f, 0.27f, 0.9f, 1.f});
    g.fillRect({box.x, box.y, 8.f, box.h});

    g.setColour(UI::defaultTheme().dimText);
    g.drawText(caption, box, UI::Justification::Centred);
}

// One card drawn into a layer of its own and placed by a matrix. The layer
// lives here, as it has to: the host fills every layer in the tree before the
// frame's own pass opens, so it is a member of the component that draws it.
struct Turned final : UI::Component
{
    Turned()
        : layer(*this)
    {
        layer.onPaint = [this](UI::Graphics& g) { paintCard(g, cardBox(), caption); };
    }

    Rect cardBox() const { return {room, room, cardWidth, cardHeight}; }

    Point middle() const
    {
        return {room + cardWidth * 0.5f, room + cardHeight * 0.5f};
    }

    void resized() override { layer.setBounds(getLocalBounds()); }

    void setPlacement(const GPUWidgets::AffineTransform& transform)
    {
        layer.setTransform(about(transform, middle()));
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    UI::Layer layer;
    std::string caption;
};

// A layer inside a layer, each turned. The inner one is constructed first
// because the host fills them in the order they registered, and an outer layer
// drawing an inner one that has not been filled yet draws nothing where it is.
//
// And each matrix is read in its own layer's space, so the inner card is
// turned about its own middle wherever the outer one carried it -- which is
// what makes a tree of them compose.
struct Nested final : UI::Component
{
    Nested()
        : inner(*this)
        , outer(*this)
    {
        inner.onPaint = [this](UI::Graphics& g)
        { paintCard(g, innerBox(), "inner"); };

        outer.onPaint = [this](UI::Graphics& g)
        {
            paintCard(g, outerBox(), "outer");
            g.drawLayer(inner);
        };
    }

    Rect outerBox() const { return {room, room, cardWidth, cardHeight}; }

    Rect innerBox() const
    {
        return {room + 26.f, room + cardHeight - 18.f, cardWidth - 52.f, 44.f};
    }

    void resized() override
    {
        outer.setBounds(getLocalBounds());
        inner.setBounds(getLocalBounds());

        outer.setTransform(about(GPUWidgets::AffineTransform::rotation(-0.35f),
                                 {room + cardWidth * 0.5f, room + cardHeight * 0.5f}));

        inner.setTransform(about(GPUWidgets::AffineTransform::rotation(0.7f),
                                 {innerBox().x + innerBox().w * 0.5f,
                                  innerBox().y + innerBox().h * 0.5f}));
    }

    void paint(UI::Graphics& g) override { g.drawLayer(outer); }

    UI::Layer inner;
    UI::Layer outer;
};

struct Label final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Left);
    }

    std::string text;
};

struct Stats final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto text = std::to_string(host->getLastRenderedLayerCount())
                    + " layers rendered last frame   "
                    + std::to_string(host->getLastComponentCount()) + " components   "
                    + std::to_string(host->getLastClipChangeCount())
                    + " clip changes";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Left);
    }

    UI::ComponentHost* host = nullptr;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        auto add = [&](std::string caption)
        {
            auto card = OwningPointer<Turned> {};
            card.create();
            card->caption = std::move(caption);
            addAndMakeVisible(*card);
            cards.add(std::move(card));
        };

        for (auto degrees: {0, 15, 30, 45, 90})
            add(std::to_string(degrees) + "°");

        for (auto caption: {"0.6x", "1.4x", "skew x", "skew y", "flipped"})
            add(caption);

        for (auto* text: {"A rotation is four corners of the same texture",
                          "A scale magnifies what was drawn; a skew leans it",
                          "A layer inside a layer, each turned in its own space"})
        {
            auto label = OwningPointer<Label> {};
            label.create();
            label->text = text;
            addAndMakeVisible(*label);
            labels.add(std::move(label));
        }

        addAndMakeVisible(nested);
        addAndMakeVisible(spinner);
        addAndMakeVisible(stats);

        spinner.caption = "spinning";
    }

    void place()
    {
        using Transform = GPUWidgets::AffineTransform;

        auto degrees = 0;

        for (auto turn: {0.f, 15.f, 30.f, 45.f, 90.f})
            cards[degrees++]->setPlacement(
                Transform::rotation(turn * 3.14159265f / 180.f));

        cards[5]->setPlacement(Transform::scaling(0.6f, 0.6f));
        cards[6]->setPlacement(Transform::scaling(1.4f, 1.4f));
        cards[7]->setPlacement(Transform::skew(0.35f, 0.f));
        cards[8]->setPlacement(Transform::skew(0.f, 0.35f));
        cards[9]->setPlacement(Transform::scaling(-1.f, 1.f));
    }

    void resized() override
    {
        auto slot = Point {cardWidth + room * 2.f, cardHeight + room * 2.f};
        auto y = margin;

        auto row = [&](int first, int count, int labelIndex)
        {
            labels[labelIndex]->setBounds({margin + room, y, 520.f, 18.f});
            y += 8.f;

            for (auto i = 0; i < count; ++i)
                cards[first + i]->setBounds(
                    {margin + slot.x * (float) i, y, slot.x, slot.y});

            y += slot.y;
        };

        row(0, 5, 0);
        row(5, 5, 1);

        labels[2]->setBounds({margin + room, y, 520.f, 18.f});
        y += 8.f;

        nested.setBounds({margin, y, slot.x, slot.y});
        spinner.setBounds({margin + slot.x, y, slot.x, slot.y});

        stats.setBounds({margin + room, getHeight() - 26.f, 620.f, 18.f});

        place();
    }

    void paint(UI::Graphics& g) override { g.fillAll(UI::defaultTheme().background); }

    Vector<OwningPointer<Label>> labels;
    Vector<OwningPointer<Turned>> cards;
    Nested nested;
    Turned spinner;
    Stats stats;
};

// The spin is the demonstration: the layer's texture is rendered once and the
// matrix is read where it is composited, so a frame of it costs a quad and the
// count in the footer stays at nothing.
struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.stats.host = this;
        setRootComponent(root);
        setContinuous(true);
    }

    void update(Threads::FrameTime frame) override
    {
        angle += (float) frame.delta;

        root.spinner.setPlacement(GPUWidgets::AffineTransform::rotation(angle));
        root.stats.repaint();
    }

    DemoRoot root;
    float angle = 0.f;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1250;
    options.height = 780;
    options.title = "eacp UI — transforms";
    options.minWidth = 480;
    options.minHeight = 320;

    return options;
}

struct App
{
    App() { window.setContentView(host); }

    DemoHost host;
    Graphics::Window window {makeOptions()};
};

// The same tree rendered to a file with no window, which is how the picture is
// looked at on a machine with nobody at the screen.
int snapshot(const char* path)
{
    auto host = DemoHost {};
    host.setBounds({0.f, 0.f, 1250.f, 780.f});
    host.resized();

    // Twice: the footer reads the last frame's figures, and the layers are
    // rendered by the first of them.
    host.renderToImage(host.backingScale());
    host.root.stats.repaint();

    auto image = host.renderToImage(host.backingScale());

    if (!image.isValid())
        return 1;

    image.save(FilePath {path});

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        return snapshot(argv[1]);

    return eacp::Apps::run<App>();
}
