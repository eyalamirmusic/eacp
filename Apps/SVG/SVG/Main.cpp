#include <eacp/SVG/SVG.h>
#include <eacp/Graphics/Tray/TrayIcon.h>

using namespace eacp;

static constexpr std::string_view logoSvgMarkup()
{
    return R"SVG(
<svg width="585" height="510" viewBox="0 0 585 510" fill="none" xmlns="http://www.w3.org/2000/svg">
    <path d="M193.823 193.365V27H27V193.365" stroke="black" stroke-width="54"/>
    <path d="M376.159 509.48V426.527C376.159 380.46 338.814 343.115 292.747 343.115C246.681 343.115 209.336 380.46 209.336 426.527V509.48" stroke="black" stroke-width="54"/>
    <path d="M558 193.365V27H391.177V193.365" stroke="black" stroke-width="54"/>
    <path d="M197.445 13.622L292.413 182.393L387.709 13.622" stroke="black" stroke-width="54" stroke-linejoin="bevel"/>
    <path d="M193.723 192.984C193.723 247.439 237.979 291.584 292.571 291.584C347.163 291.584 391.177 247.687 391.177 193.231" stroke="black" stroke-width="54"/>
</svg>
)SVG";
}

// Every bundled SVG rasterized through SVG::toImage at a run of heights
// and painted with Context::drawImage — a farm of raster renders beneath
// the live vector SVGView.
struct SvgFarm final : Graphics::View
{
    struct Entry
    {
        std::string name;
        Vector<Graphics::Image> images;
    };

    void addEntry(const std::string& name, const std::string& markup)
    {
        auto entry = Entry {name, {}};
        for (auto height: {12, 24, 48, 96})
        {
            auto image = SVG::toImage(markup, 0, height);
            if (image.isValid())
                entry.images.add(std::move(image));
        }

        if (!entry.images.empty())
            entries.add(std::move(entry));

        repaint();
    }

    void paint(Graphics::Context& context) override
    {
        auto bounds = getLocalBounds();
        context.setColor(Graphics::Color::gray(0.95f));
        context.fillRect(bounds);

        constexpr auto cellWidth = 262.f;
        constexpr auto cellHeight = 132.f;
        auto columns = std::max(1, static_cast<int>(bounds.w / cellWidth));

        auto font = Graphics::Font(
            Graphics::FontOptions().withName("Helvetica").withSize(11.f));

        for (auto i = 0; i < entries.size(); ++i)
        {
            auto& entry = entries[i];
            auto cellX = static_cast<float>(i % columns) * cellWidth + 12.f;
            auto baseline = static_cast<float>(i / columns) * cellHeight + 104.f;

            auto x = cellX;
            for (auto& image: entry.images)
            {
                auto w = static_cast<float>(image.width());
                auto h = static_cast<float>(image.height());
                context.drawImage(image, {x, baseline - h, w, h});
                x += w + 8.f;
            }

            context.setColor(Graphics::Color::gray(0.35f));
            context.drawText(entry.name, {cellX, baseline + 16.f}, font);
        }
    }

    Vector<Entry> entries;
};

struct MainView final : Graphics::View
{
    void resized() override
    {
        auto bounds = getLocalBounds();
        auto vectorBounds = bounds.removeFromTop(170.f);

        if (vectorView != nullptr)
            vectorView->setBounds(vectorBounds.inset(8.f));
        farm.setBounds(bounds);
    }

    Graphics::View* vectorView = nullptr;
    SvgFarm farm;
};

static Graphics::WindowOptions getWindowOptions()
{
    auto options = Graphics::WindowOptions();
    options.width = 810;
    options.height = 720;
    options.title = "SVG Farm";
    return options;
}

struct MyApp
{
    MyApp()
    {
        auto contents = Files::readFile(Files::getBundleResourcePath("example.svg"));
        result = SVG::parse(contents);
        if (result.root)
        {
            result.root->stretchToFit();
            main.vectorView = result.root;
            main.addSubview(*result.root);
        }

        main.farm.addEntry("example.svg", contents);
        for (auto* name: {"bootstrap-camera.svg",
                          "bootstrap-heart-fill.svg",
                          "feather-activity.svg",
                          "feather-check-circle.svg",
                          "flag-fr.svg",
                          "flag-jp.svg",
                          "material-home.svg",
                          "simpleicons-github.svg",
                          "tabler-star.svg",
                          "twemoji-smile.svg"})
            main.farm.addEntry(name,
                               Files::readFile(Files::getBundleResourcePath(name)));

        main.addSubview(main.farm);
        window.setContentView(main);

        trayIcon.setIcon(SVG::toImage(std::string {logoSvgMarkup()}, 36, 36));
        trayIcon.setTooltip("SVG App");
    }

    SVG::ParseResult result;
    MainView main;
    Graphics::Window window {getWindowOptions()};
    Graphics::TrayIcon trayIcon;
};

int main()
{
    Apps::run<MyApp>();

    return 0;
}
