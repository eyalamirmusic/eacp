#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <string_view>

// The one part of the module that needs a device: a detached ComponentHost
// rendered off-screen and read back. Its own executable so SVGTests stays the
// window-free suite Tests/CMakeLists.txt says it is.

using namespace nano;
using namespace eacp;

namespace
{
constexpr auto redSquare = std::string_view {
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">)"
    R"(<rect width="10" height="10" fill="#ff0000"/></svg>)"};

constexpr auto wideBar = std::string_view {
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">)"
    R"(<rect width="20" height="10" fill="#0000ff"/></svg>)"};

bool isOpaque(const Graphics::Color& pixel)
{
    return pixel.a > 0.9f;
}

bool isClear(const Graphics::Color& pixel)
{
    return pixel.a < 0.1f;
}
} // namespace

auto tFillsRequestedSize = test("SVGImage/fillsTheRequestedPixelSize") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = SVG::renderToImage(redSquare, 16, 16);

    check(image.isValid());
    check(image.width() == 16);
    check(image.height() == 16);
};

auto tKeepsDocumentColour = test("SVGImage/keepsTheDocumentsOwnColour") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = SVG::renderToImage(redSquare, 16, 16);
    check(image.isValid());

    auto centre = image.at(8, 8);

    check(centre.r > 0.9f);
    check(centre.g < 0.1f);
    check(centre.b < 0.1f);
    check(isOpaque(centre));
};

// preserveAspectRatio defaults to uniform and centred, so a 2:1 document in a
// square lands as a band with clear rows above and below it.
auto tLetterboxesRatherThanStretches = test("SVGImage/letterboxesADocument") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = SVG::renderToImage(wideBar, 40, 40);
    check(image.isValid());

    check(isClear(image.at(20, 2)));
    check(isOpaque(image.at(20, 20)));
    check(isClear(image.at(20, 37)));
};

auto tBackgroundIsTransparent = test("SVGImage/leavesTheBackgroundTransparent") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto document = std::string_view {
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">)"
        R"(<rect x="4" y="4" width="2" height="2" fill="#00ff00"/></svg>)"};

    auto image = SVG::renderToImage(document, 20, 20);
    check(image.isValid());

    check(isClear(image.at(1, 1)));
    check(isOpaque(image.at(10, 10)));
};

auto tRejectsBadInput = test("SVGImage/rejectsUnparseableMarkupAndEmptySizes") = []
{
    check(!SVG::renderToImage(std::string_view {"not markup at all"}, 16, 16)
               .isValid());
    check(!SVG::renderToImage(redSquare, 0, 16).isValid());
    check(!SVG::renderToImage(redSquare, 16, -1).isValid());
};

namespace
{
// A clip-path in a document that has a viewBox: the clip is authored in the
// document's units, the same as the shape it cuts.
constexpr auto clippedUnderViewBox = std::string_view {
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 140 22">)svg"
    R"svg(<rect clip-path="url(#a)" width="140" height="22" fill="#0000ff"/>)svg"
    R"svg(<defs><clipPath id="a"><path d="M0 0h140v21.42H0z"/></clipPath></defs></svg>)svg"};
} // namespace

auto tClipUnderViewBox = test("SVGImage/aClipPathIsInTheViewBoxsUnits") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = SVG::renderToImage(clippedUnderViewBox, 140, 22);
    check(image.isValid());

    auto centre = image.at(70, 11);

    check(isOpaque(centre), "the clip covers the shape, so the shape draws");
    check(centre.b > 0.9f);

    auto scaled = SVG::renderToImage(clippedUnderViewBox, 280, 44);
    check(isOpaque(scaled.at(140, 22)), "and at twice the size");

    // Letterboxed: the viewBox is centred in a taller picture, so the whole
    // document - clip included - moves down.
    auto again = SVG::renderToImage(clippedUnderViewBox, 280, 44);
    check(isOpaque(again.at(140, 22)), "and a second time, in a host of its own");

    auto wide = SVG::renderToImage(clippedUnderViewBox, 300, 47);
    check(isOpaque(wide.at(150, 23)), "at a fractional scale with no letterbox");

    auto tall = SVG::renderToImage(clippedUnderViewBox, 140, 44);
    check(isOpaque(tall.at(70, 22)), "letterboxed at scale one");

    auto boxed = SVG::renderToImage(clippedUnderViewBox, 300, 150);
    check(isOpaque(boxed.at(150, 75)), "and moved by the letterbox");
    check(isClear(boxed.at(150, 5)));
};
