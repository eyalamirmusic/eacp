#include <eacp/SVG/SVG.h>
#include <eacp/UI/UI.h>

#include <ResEmbed/ResEmbed.h>

using namespace eacp;

namespace
{
struct DocumentView final : UI::ComponentHost
{
    DocumentView()
    {
        setBackgroundColour(Graphics::Color::white());
        loadDocument();
        setRootComponent(document);
    }

    void loadDocument()
    {
        auto file = ResEmbed::get("example.svg", "SVGAssets");

        if (!file)
        {
            LOG("SVG: example.svg is not embedded");
            return;
        }

        auto parsed = SVG::parseXML(file.toStringView());

        if (!parsed.has_value())
        {
            LOG("SVG: example.svg did not parse");
            return;
        }

        document.setDocument(*parsed);
    }

    SVG::SVGComponent document;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 400;
    options.height = 400;
    options.title = "eacp SVG";

    return options;
}
} // namespace

int main()
{
    return Graphics::runWindowedApp<DocumentView>(makeOptions());
}
