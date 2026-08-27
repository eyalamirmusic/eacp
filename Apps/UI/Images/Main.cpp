#include <eacp/UI/UI.h>

#include <cmath>
#include <string>

// Pictures in the component tier, and what each of them costs.
//
// An image is drawn as a quad of its texture by the same kind of instanced
// batch the shapes go out in, grouped by texture: a row of icons out of one
// image is one draw, a change of image is one more, and a shape between two
// images is a switch between renderers that the footer counts alongside the
// clip changes. Everything is drawn through the clip in force, so the photo
// under a rounded corner is cut by the corner and not by its box.
//
// The top row is one image at every size down from its own. It is sampled
// through its mip chain, so the small copies read a level near their size
// rather than a scatter of texels -- the checker in it would otherwise alias
// into moire at 64 and vanish into noise below that.

using namespace eacp;

namespace
{
constexpr auto padding = 16.f;
constexpr auto photoSize = 512;
constexpr auto iconSize = 32;
constexpr auto iconCount = 40;

// A picture with something at every scale: a fine checker under a smooth sweep
// of colour, so drawing it below its own size shows whether the levels are
// read or the texels are skipped.
Graphics::Image makePhoto(int size)
{
    auto image = Graphics::Image {size, size};
    auto last = (float) (size - 1);

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
        {
            auto checker = ((x / 4 + y / 4) % 2 == 0) ? 1.f : 0.6f;
            auto fx = (float) x / last;
            auto fy = (float) y / last;

            image.set(x,
                      y,
                      {checker * (0.35f + 0.65f * fx),
                       checker * (0.45f + 0.55f * fy),
                       checker * (1.f - 0.6f * fx),
                       1.f});
        }
    }

    return image;
}

// A ring with a transparent margin: an icon, and a check that straight alpha
// composites as it should over whatever it lands on.
Graphics::Image makeIcon(int size, const Graphics::Color& colour)
{
    auto image = Graphics::Image {size, size};
    auto centre = (float) size * 0.5f;
    auto outer = centre - 2.f;
    auto inner = outer * 0.55f;

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
        {
            auto dx = (float) x + 0.5f - centre;
            auto dy = (float) y + 0.5f - centre;
            auto distance = std::sqrt(dx * dx + dy * dy);

            auto coverage = std::clamp(outer - distance + 0.5f, 0.f, 1.f)
                            * std::clamp(distance - inner + 0.5f, 0.f, 1.f);

            image.set(x, y, {colour.r, colour.g, colour.b, colour.a * coverage});
        }
    }

    return image;
}

struct Gallery final : UI::Component
{
    Gallery()
    {
        // A box this size is past the point where a shape is worth a mask, so
        // left to itself it would mesh -- and a meshed shape clips to its
        // bounds and no further, since a mesh carries no coverage to sample.
        // The corners are the point here, so the mask is asked for.
        corners.setBacking(UI::PathShape::Backing::Mask);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto clipped = Graphics::Rect {area.x, area.y + 580.f, 200.f, 140.f};

        auto path = GPUWidgets::Path {};
        path.addRoundedRect(clipped, 28.f);
        corners.setPath(path);
    }

    void paint(UI::Graphics& g) override
    {
        const auto& theme = UI::defaultTheme();

        // Kept, rather than asked for on every paint: the cache finds an image
        // by hashing its pixels, and half a megapixel is worth hashing once.
        if (photo == nullptr)
        {
            auto& cache = getHost()->getImageCache();

            photo = cache.get(makePhoto(photoSize));
            icon = cache.get(makeIcon(iconSize, theme.accent));
        }

        auto x = 0.f;

        // The same image at every size down from its own.
        for (auto size = (float) photoSize; size >= 8.f; size *= 0.5f)
        {
            g.drawImage(photo, {x, 0.f, size, size});
            x += size + padding;
        }

        g.setColour(theme.dimText);
        g.drawText("one image, 512 to 8 points, through its mip chain",
                   {0.f, 512.f + padding, 600.f, 20.f});

        // A row of icons out of one texture: one draw however long the row.
        for (auto i = 0; i < iconCount; ++i)
            g.drawImage(icon,
                        {(float) i * (iconSize + 4.f), 548.f, iconSize, iconSize});

        auto row = 580.f;

        // Under a rounded clip, and beside it the same box as a plain fill.
        {
            auto state = UI::Graphics::ScopedState {g};
            g.reduceClipToShape(corners);
            g.drawImage(photo, {0.f, row, 200.f, 140.f});
        }

        g.setColour(theme.panel);
        g.fillRoundedRect({216.f, row, 200.f, 140.f}, 28.f);

        // A crop: the middle quarter, stretched.
        g.drawImage(photo, {128.f, 128.f, 256.f, 256.f}, {432.f, row, 140.f, 140.f});

        // Faded under a caption, which is what an opacity is for. Text goes
        // above the images of its clip region whatever order it was issued in,
        // as it does above the shapes -- so this is the order to draw it in.
        g.drawImage(photo, {588.f, row, 300.f, 140.f}, 0.4f);
        g.setColour(theme.text);
        g.setFontSize(28.f);
        g.drawText("text over a faded image", {588.f, row, 300.f, 140.f});
    }

    UI::ImageRef photo;
    UI::ImageRef icon;
    UI::PathShape corners {*this};
};

// Reads the host's figures at paint time, for the reason ComponentTree's does:
// a repaint asked for from inside the frame that produced them is cleared on the
// way out, so anything derived from a frame has to be read during the next one.
struct StatsBar final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        if (host == nullptr)
            return;

        auto text =
            std::to_string(host->getLastComponentCount()) + " components   "
            + std::to_string(host->getLastClipChangeCount()) + " clip changes   "
            + std::to_string(host->getLastRendererSwitchCount())
            + " renderer switches   " + std::to_string(host->getLastImageDrawCount())
            + " image draws   " + std::to_string(host->getCachedImageCount())
            + " textures";

        g.setColour(UI::defaultTheme().dimText);
        g.drawText(text, getLocalBounds(), UI::Justification::Right);

        if (text != lastPainted)
        {
            lastPainted = text;
            Threads::callAsync([this] { repaint(); });
        }
    }

    UI::ComponentHost* host = nullptr;
    std::string lastPainted;
};

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        title.setColour(UI::defaultTheme().text);
        addChildren({title, gallery, stats});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        title.setBounds(area.removeFromTop(28.f));
        area.removeFromTop(padding);

        stats.setBounds(area.removeFromBottom(22.f));
        area.removeFromBottom(6.f);

        gallery.setBounds(area);
    }

    UI::Label title {"eacp UI — images, batched by texture"};
    Gallery gallery;
    StatsBar stats;
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
    options.width = 1100;
    options.height = 820;
    options.title = "eacp UI — images";
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
// looked at on a machine with nobody at the screen. Sized and told resized() by
// hand, since a view in no window is never told and that is where the host
// builds its renderers.
int snapshot(const char* path)
{
    auto host = DemoHost {};
    host.setBounds({0.f, 0.f, 1100.f, 820.f});
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
