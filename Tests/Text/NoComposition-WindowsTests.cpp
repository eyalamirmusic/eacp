#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Graphics.h>

// Everything that should still work when DirectComposition does not.
//
// A session with no desktop compositor - a service, a CI runner, a job started
// over ssh into session 0 - fails DCompositionCreateDevice2 with
// E_ACCESSDENIED. Nothing can reach the screen then, but nothing in text or in
// an off-screen snapshot needs the compositor: DirectWrite measures and
// shapes, and renderToImage draws through a plain Direct2D device context. So
// those have to keep working, and the code that uses them must not crash.
//
// Its own binary because the compositor is a process-wide singleton built on
// first use: main sets EACP_DCOMP_UNAVAILABLE before anything can build it,
// which makes composition-device creation fail exactly as such a session does
// (see DComp-Windows.h), on a desktop that has a compositor as on one that has
// none.

using namespace nano;
using namespace eacp;

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}

Text::FontRequest uiFontRequest()
{
    auto request = Text::FontRequest {};
    request.family = "Segoe UI";
    request.pointSize = 16.f;
    request.scale = 1.f;

    return request;
}

Graphics::Font uiFont()
{
    auto options = Graphics::FontOptions {};
    options.name = "Segoe UI";
    options.size = 16.f;

    return Graphics::Font {options};
}

// White text on black, so any pixel brighter than mid-grey is ink.
struct TextSwatch final : Graphics::View
{
    TextSwatch() { setBounds({0.f, 0.f, 120.f, 40.f}); }

    void paint(Graphics::Context& g) override
    {
        g.setColor({0.f, 0.f, 0.f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor({1.f, 1.f, 1.f, 1.f});
        g.drawText("Hello", {8.f, 8.f}, font);
    }

    Graphics::Font font = uiFont();
};

int inkPixels(const Graphics::Image& image)
{
    auto count = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (image.at(x, y).r > 0.5f)
                ++count;

    return count;
}
} // namespace

// The seam took: no composition device, and the rest of the stack up anyway.
auto tNoCompositionDevice = test("NoComposition/onlyTheCompositorIsMissing") = []
{
    check(!Graphics::isCompositorInitialized());
    check(Graphics::getCompositionDevice() == nullptr);

    check(Graphics::getDWriteFactory() != nullptr);
    check(Graphics::getD2DFactory() != nullptr);
    check(Graphics::getD2DDevice() != nullptr);
};

auto tTextMetrics = test("NoComposition/textMetricsMeasure") = []
{
    const auto font = uiFont();

    check(Graphics::TextMetrics::measureWidth("Hello", font) > 0.f);
    check(Graphics::TextMetrics::getLineHeight(font) > 0.f);
    check(Graphics::TextMetrics::getAscent(font) > 0.f);
};

auto tRasterizerShapes = test("NoComposition/rasterizerShapes") = []
{
    const auto rasterizer = Text::GlyphRasterizer(uiFontRequest());

    check(rasterizer.isValid());
    check(rasterizer.metrics({}).ascent > 0.f);

    const auto run = rasterizer.shape("Hello", {});

    check(run.glyphs.size() == 5);
    check(run.advance > 0.f);
};

auto tRenderToImage = test("NoComposition/renderToImageDrawsText") = []
{
    auto view = TextSwatch {};
    const auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 120);
    check(image.height() == 40);
    check(inkPixels(image) > 0);
};

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    setEnv("EACP_DCOMP_UNAVAILABLE", "1");

    Apps::getAppEnvironment().headless = true;
    Apps::run(runTests);

    return exitCode;
}
