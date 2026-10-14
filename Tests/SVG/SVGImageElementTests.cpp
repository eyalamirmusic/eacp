#include <eacp/Core/Utils/Base64.h>
#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;

namespace SVGImageElementTests
{
namespace
{
const auto quadrants = std::string {
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEklEQVR42mP4z8DAAMIM/4EAAB/uBfvxq7p3AAAAAElFTkSuQmCC"};

bool canDraw()
{
    if (!GPU::Device::shared().isValid())
        return false;

    auto bytes = Base64::decode(quadrants.substr(quadrants.find(',') + 1));

    return bytes.has_value()
           && Graphics::Image::decode(
                  reinterpret_cast<const std::uint8_t*>(bytes->data()),
                  static_cast<int>(bytes->size()))
                  .isValid();
}

std::string document(const std::string& body)
{
    return R"(<svg xmlns="http://www.w3.org/2000/svg" )"
           R"(xmlns:xlink="http://www.w3.org/1999/xlink" width="40" height="40">)"
           + body + "</svg>";
}

Graphics::Image render(const std::string& body)
{
    return SVG::renderToImage(std::string_view {document(body)}, 40, 40);
}

std::string image(const std::string& attributes, const std::string& href = quadrants)
{
    return R"(<image href=")" + href + R"(" )" + attributes + "/>";
}

bool isClear(const Graphics::Color& pixel)
{
    return pixel.a < 0.1f;
}

bool near(const Graphics::Color& pixel, float r, float g, float b)
{
    return pixel.a > 0.9f && std::abs(pixel.r - r) < 0.15f
           && std::abs(pixel.g - g) < 0.15f && std::abs(pixel.b - b) < 0.15f;
}

bool isRed(const Graphics::Color& pixel)
{
    return near(pixel, 1.f, 0.f, 0.f);
}

bool isGreen(const Graphics::Color& pixel)
{
    return near(pixel, 0.f, 1.f, 0.f);
}

bool isBlue(const Graphics::Color& pixel)
{
    return near(pixel, 0.f, 0.f, 1.f);
}

bool isWhite(const Graphics::Color& pixel)
{
    return near(pixel, 1.f, 1.f, 1.f);
}

std::string patternOfQuadrants()
{
    return R"svg(<defs><pattern id="p" patternContentUnits="objectBoundingBox" )svg"
           R"svg(width="1" height="1"><use xlink:href="#i" transform="scale(0.5)"/>)svg"
           R"svg(</pattern><image id="i" width="2" height="2" xlink:href=")svg"
           + quadrants + R"svg("/></defs>)svg";
}

int imageCount(const std::string& body)
{
    auto root = SVG::parseXML(document(body));
    auto component = SVG::SVGComponent {};

    component.setBounds({0.f, 0.f, 40.f, 40.f});

    if (root.has_value())
        component.setDocument(*root);

    return component.getImageCount();
}
} // namespace

auto tDrawsDecodedPixels =
    test("SVGImageElement/drawsTheDecodedPixelsWhereItIsPlaced") = []
{
    if (!canDraw())
        return;

    auto picture = render(image(R"(x="10" y="10" width="20" height="20")"));
    check(picture.isValid());

    check(isRed(picture.at(12, 12)));
    check(isGreen(picture.at(27, 12)));
    check(isBlue(picture.at(12, 27)));
    check(isWhite(picture.at(27, 27)));

    check(isClear(picture.at(5, 5)));
    check(isClear(picture.at(35, 20)));
    check(isClear(picture.at(20, 35)));
};

auto tIntrinsicSize = test("SVGImageElement/takesItsOwnSizeWhenGivenNone") = []
{
    if (!canDraw())
        return;

    auto picture = render(image(R"(x="4" y="4")"));
    check(picture.isValid());

    check(isRed(picture.at(4, 4)));
    check(isWhite(picture.at(5, 5)));
    check(isClear(picture.at(8, 8)));
};

auto tLetterboxesByDefault =
    test("SVGImageElement/letterboxesIntoAWiderBoxByDefault") = []
{
    if (!canDraw())
        return;

    auto picture = render(image(R"(width="40" height="20")"));
    check(picture.isValid());

    check(isClear(picture.at(5, 10)));
    check(isClear(picture.at(35, 10)));
    check(isRed(picture.at(12, 2)));
    check(isWhite(picture.at(27, 17)));
    check(isClear(picture.at(20, 30)));
};

auto tAspectRatioNoneStretches =
    test("SVGImageElement/preserveAspectRatioNoneStretches") = []
{
    if (!canDraw())
        return;

    auto picture =
        render(image(R"(width="40" height="20" preserveAspectRatio="none")"));
    check(picture.isValid());

    check(isRed(picture.at(2, 2)));
    check(isGreen(picture.at(37, 2)));
    check(isBlue(picture.at(2, 17)));
    check(isWhite(picture.at(37, 17)));
    check(isClear(picture.at(20, 30)));
};

auto tSliceCovers = test("SVGImageElement/sliceCoversAndIsCutToItsBox") = []
{
    if (!canDraw())
        return;

    auto picture = render(
        image(R"(width="40" height="20" preserveAspectRatio="xMidYMid slice")"));
    check(picture.isValid());

    check(isRed(picture.at(2, 1)));
    check(isGreen(picture.at(37, 1)));
    check(isClear(picture.at(20, 30)));
};

auto tInheritsTransform = test("SVGImageElement/isPlacedByTheTransformsAboveIt") = []
{
    if (!canDraw())
        return;

    auto picture =
        render(R"svg(<g transform="translate(20 0)"><g transform="scale(0.5)">)svg"
               + image(R"(width="40" height="40")") + "</g></g>");
    check(picture.isValid());

    check(isClear(picture.at(5, 5)));
    check(isRed(picture.at(22, 2)));
    check(isWhite(picture.at(37, 17)));
    check(isClear(picture.at(30, 30)));
};

auto tXlinkHref = test("SVGImageElement/readsTheXlinkSpellingOfHref") = []
{
    if (!canDraw())
        return;

    auto picture = render(R"(<image xlink:href=")" + quadrants
                          + R"(" width="40" height="40"/>)");
    check(picture.isValid());

    check(isRed(picture.at(5, 5)));
    check(isWhite(picture.at(35, 35)));
};

auto tJpegPayload = test("SVGImageElement/decodesAJpegPayload") = []
{
    if (!canDraw())
        return;

    auto source = Graphics::Image {8, 8};

    for (auto y = 0; y < 8; ++y)
        for (auto x = 0; x < 8; ++x)
            source.set(x, y, {0.f, 0.f, 1.f, 1.f});

    auto bytes = source.toJpeg(1.f);
    auto encoded = Base64::encode({reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<std::size_t>(bytes.size())});

    auto picture = render(
        image(R"(width="40" height="40")", "data:image/jpeg;base64," + encoded));
    check(picture.isValid());

    check(isBlue(picture.at(20, 20)));
};

auto tUnusableHrefDrawsNothing =
    test("SVGImageElement/aMissingOrNonDataHrefDrawsNothingElse") = []
{
    if (!canDraw())
        return;

    auto rect = std::string {
        R"(<rect x="30" y="30" width="10" height="10" fill="#00ff00"/>)"};

    for (auto& href: {std::string {"photo.png"},
                      std::string {"https://example.com/photo.png"},
                      std::string {"data:image/png;base64,!!!!"},
                      std::string {"data:text/plain;base64,aGVsbG8="}})
    {
        auto picture = render(image(R"(width="20" height="20")", href) + rect);
        check(picture.isValid());

        check(isClear(picture.at(10, 10)), href);
        check(isGreen(picture.at(35, 35)), "the rest of the document still draws");
        check(imageCount(image(R"(width="20" height="20")", href)) == 0);
    }

    auto unlinked = render(R"(<image width="20" height="20"/>)" + rect);

    check(isClear(unlinked.at(10, 10)));
    check(isGreen(unlinked.at(35, 35)));
};

auto tOpacityFades = test("SVGImageElement/opacityFadesTheImage") = []
{
    if (!canDraw())
        return;

    auto own = render(image(R"(width="40" height="40" opacity="0.5")"));
    check(own.isValid());

    auto pixel = own.at(5, 5);

    check(pixel.a > 0.4f && pixel.a < 0.6f);
    check(pixel.r > 0.8f && pixel.g < 0.2f);

    auto group =
        render(R"(<g opacity="0.5">)" + image(R"(width="40" height="40")") + "</g>");
    auto grouped = group.at(5, 5);

    check(grouped.a > 0.4f && grouped.a < 0.6f, "and so does its group's");
};

auto tHiddenDrawsNothing =
    test("SVGImageElement/displayNoneAndVisibilityHiddenDrawNothing") = []
{
    if (!canDraw())
        return;

    auto hidden = render(image(R"(width="40" height="40" visibility="hidden")"));
    check(isClear(hidden.at(20, 20)));

    auto undisplayed =
        render(image(R"(width="40" height="40" style="display:none")"));
    check(isClear(undisplayed.at(20, 20)));

    auto inGroup = render(R"(<g display="none">)"
                          + image(R"(width="40" height="40")") + "</g>");
    check(isClear(inGroup.at(20, 20)));

    auto shown =
        render(R"(<g visibility="hidden">)"
               + image(R"(width="40" height="40" visibility="visible")") + "</g>");
    check(isRed(shown.at(5, 5)));
};

auto tClipPathCuts = test("SVGImageElement/isCutByAClipPath") = []
{
    if (!canDraw())
        return;

    auto picture =
        render(R"(<clipPath id="c"><rect width="20" height="40"/></clipPath>)"
               + image(R"svg(width="40" height="40" clip-path="url(#c)")svg"));
    check(picture.isValid());

    check(isRed(picture.at(5, 5)));
    check(isClear(picture.at(35, 5)));
};

auto tPatternFill =
    test("SVGImageElement/aPatternOfAnImageFillsTheShapeThatNamesIt") = []
{
    if (!canDraw())
        return;

    auto picture =
        render(R"svg(<rect x="10" y="10" width="20" height="20" fill="url(#p)"/>)svg"
               + patternOfQuadrants());
    check(picture.isValid());

    check(isRed(picture.at(12, 12)));
    check(isGreen(picture.at(27, 12)));
    check(isBlue(picture.at(12, 27)));
    check(isWhite(picture.at(27, 27)));
    check(isClear(picture.at(5, 5)), "the image in <defs> draws nowhere itself");
    check(isClear(picture.at(35, 35)));
};

auto tPatternIsCutToTheShape =
    test("SVGImageElement/aPatternIsCutToTheShapeAndFadedByItsFillOpacity") = []
{
    if (!canDraw())
        return;

    auto picture = render(
        R"svg(<circle cx="20" cy="20" r="20" fill="url(#p)" fill-opacity="0.5"/>)svg"
        + patternOfQuadrants());
    check(picture.isValid());

    check(isClear(picture.at(1, 1)), "outside the circle");

    auto inside = picture.at(10, 10);

    check(inside.a > 0.4f && inside.a < 0.6f);
    check(inside.r > 0.8f && inside.g < 0.2f);
};

auto tPatternRepeats =
    test("SVGImageElement/aPatternRepeatsItsTileAcrossTheShape") = []
{
    if (!canDraw())
        return;

    auto picture = render(
        R"svg(<rect width="40" height="40" fill="url(#t)"/>)svg"
        R"svg(<defs><pattern id="t" patternUnits="userSpaceOnUse" width="20" height="20">)svg"
        + image(R"(width="20" height="20")") + "</pattern></defs>");
    check(picture.isValid());

    check(isRed(picture.at(2, 2)));
    check(isRed(picture.at(22, 2)));
    check(isRed(picture.at(2, 22)));
    check(isWhite(picture.at(17, 17)));
    check(isWhite(picture.at(37, 37)));
};
} // namespace SVGImageElementTests
