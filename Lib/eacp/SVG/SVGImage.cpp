#include "SVGImage.h"

#include "SVGComponent.h"
#include "XMLParser.h"

namespace eacp::SVG
{
Graphics::Image renderToImage(const SVGElement& root, int width, int height)
{
    if (width <= 0 || height <= 0 || !GPU::Device::shared().isValid())
        return {};

    // The host holds the root as a bare pointer, so the component outlives it.
    auto document = SVGComponent {};
    auto host = UI::ComponentHost {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds(
        {0.f, 0.f, static_cast<float>(width), static_cast<float>(height)});

    // The host builds its renderers in resized(), which a view outside a window
    // is never laid out to receive.
    host.resized();

    host.setRootComponent(document);
    document.setDocument(root);

    return host.renderToImage(1.f);
}

Graphics::Image renderToImage(std::string_view markup, int width, int height)
{
    auto root = parseXML(markup);

    if (!root.has_value())
        return {};

    return renderToImage(*root, width, height);
}
} // namespace eacp::SVG
