#include <eacp/UI/UI.h>

#include <string>

// Shadows in the component tier: a blurred rounded box, which is the shape
// every card, dialog and dropdown in an interface sits on.
//
// What it is here to show is that the blur costs nothing to batch. A shadow is
// the same distance field a fill is, read through a ramp as wide as the blur
// instead of one as wide as a pixel, so every card below and every shadow under
// it goes out in one instanced draw -- there is no blur kernel, no texture to
// render into and no pass of its own. The footer counts the batch breaks, and
// the number does not move when the shadows are switched off.
//
// The rows: the same card at rising blurs, at rising spreads (a shadow larger
// than the box that casts it), at rising radii, a stack of three shadows on one
// card, which is what a design system's "elevation" is, and one drawn inside
// the box instead of behind it.
//
// Nowhere is a shadow drawn under the card that casts it, which is what lets a
// translucent one show the page rather than its own shadow.

using namespace eacp;

namespace
{
using UI::Color;
using UI::Point;
using UI::Rect;

constexpr auto cardWidth = 130.f;
constexpr auto cardHeight = 84.f;
constexpr auto margin = 30.f;

// The room a card's component leaves round the card itself, which is what its
// shadow needs: paint() is handed a Graphics already clipped to the component's
// bounds, so a shadow drawn past them is a shadow cut off at them.
constexpr auto spill = 54.f;

const auto shadowColour = Color {0.f, 0.f, 0.f, 0.35f};

Rect cardBox()
{
    return {spill, spill, cardWidth, cardHeight};
}

struct Card final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto box = cardBox();

        if (blur > 0.f || spread > 0.f)
        {
            g.setColour(shadowColour);
            g.drawShadow({box, radius, offset, blur, spread});
        }

        g.setColour(UI::defaultTheme().panel);
        g.fillRoundedRect(box, radius);

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(caption, box, UI::Justification::Centred);
    }

    float blur = 0.f;
    float spread = 0.f;
    float radius = 10.f;
    Point offset {0.f, 4.f};
    std::string caption;
};

// One card carrying three shadows at once, near, middle and far, which is how
// an elevation is written and the case a single blur cannot draw.
struct StackedCard final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto box = cardBox();

        for (auto layer: {2, 1, 0})
        {
            auto depth = (float) (layer + 1);

            g.setColour(Color {0.f, 0.f, 0.f, 0.14f});
            g.drawShadow({box, 10.f, {0.f, depth * depth}, depth * depth * 1.5f});
        }

        g.setColour(UI::defaultTheme().panel);
        g.fillRoundedRect(box, 10.f);

        g.setColour(UI::defaultTheme().dimText);
        g.drawText("stacked", box, UI::Justification::Centred);
    }
};

// The same shadow the other way round: inside the box rather than behind it,
// which is what a well, a pressed button and a sunk-in field are.
struct InsetCard final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto box = cardBox();

        g.setColour(UI::defaultTheme().panel);
        g.fillRoundedRect(box, 10.f);

        g.setColour(shadowColour);
        g.drawShadow({box, 10.f, {0.f, 3.f}, 10.f, 0.f, true});

        g.setColour(UI::defaultTheme().dimText);
        g.drawText("inset", box, UI::Justification::Centred);
    }
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

        auto text = std::to_string(host->getLastComponentCount()) + " components   "
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
        auto row = [&](const char* text, auto& cards)
        {
            auto label = OwningPointer<Label> {};
            label.create();
            label->text = text;
            addAndMakeVisible(*label);
            labels.add(std::move(label));

            for (auto& card: cards)
                addAndMakeVisible(*card);
        };

        for (auto blur: {0.f, 4.f, 12.f, 24.f, 48.f})
        {
            auto card = OwningPointer<Card> {};
            card.create();
            card->blur = blur;
            card->caption = "blur " + std::to_string((int) blur);
            blurs.add(std::move(card));
        }

        for (auto spread: {0.f, 4.f, 10.f, 20.f, 32.f})
        {
            auto card = OwningPointer<Card> {};
            card.create();
            card->blur = 12.f;
            card->spread = spread;
            card->caption = "spread " + std::to_string((int) spread);
            spreads.add(std::move(card));
        }

        for (auto radius: {0.f, 6.f, 16.f, 30.f, 42.f})
        {
            auto card = OwningPointer<Card> {};
            card.create();
            card->blur = 16.f;
            card->radius = radius;
            card->caption = "radius " + std::to_string((int) radius);
            radii.add(std::move(card));
        }

        row("A blur is a ramp as wide as itself", blurs);
        row("A spread is a bigger box", spreads);
        row("Rounded, at any radius", radii);

        addAndMakeVisible(stacked);
        addAndMakeVisible(inset);
        addAndMakeVisible(stats);
    }

    void resized() override
    {
        // The component a card sits in, which is the card and the room its
        // shadow falls in.
        auto slot = Point {cardWidth + spill * 2.f, cardHeight + spill * 2.f};
        auto y = margin;

        auto layOut = [&](Vector<OwningPointer<Card>>& cards, int labelIndex)
        {
            labels[labelIndex]->setBounds({margin + spill, y, 400.f, 18.f});
            y += 8.f;

            auto x = margin;

            for (auto& card: cards)
            {
                card->setBounds({x, y, slot.x, slot.y});
                x += slot.x;
            }

            y += slot.y;
        };

        layOut(blurs, 0);
        layOut(spreads, 1);
        layOut(radii, 2);

        stacked.setBounds({margin, y, slot.x, slot.y});
        inset.setBounds({margin + slot.x, y, slot.x, slot.y});
        stats.setBounds({margin + spill, getHeight() - 26.f, 400.f, 18.f});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    Vector<OwningPointer<Label>> labels;
    Vector<OwningPointer<Card>> blurs;
    Vector<OwningPointer<Card>> spreads;
    Vector<OwningPointer<Card>> radii;
    StackedCard stacked;
    InsetCard inset;
    Stats stats;
};

struct DemoHost final : UI::ComponentHost
{
    DemoHost()
    {
        setFontPointSize(13.f);
        root.stats.host = this;
        setRootComponent(root);
    }

    DemoRoot root;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1250;
    options.height = 880;
    options.title = "eacp UI — shadows";
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
    host.setBounds({0.f, 0.f, 1250.f, 880.f});
    host.resized();

    // Twice: the footer reads the last frame's figures, and with no event loop
    // to answer its own repaint it has to be asked by hand.
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
