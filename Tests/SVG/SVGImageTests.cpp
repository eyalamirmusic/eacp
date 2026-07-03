#include <eacp/SVG/SVG.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace nano;
using eacp::Graphics::Color;
using eacp::Graphics::Image;

namespace
{
constexpr auto redSquareSvg = R"(<svg width="8" height="8">
  <rect x="0" y="0" width="8" height="8" fill="#ff0000"/>
</svg>)";

bool roughly(float value, float expected)
{
    return std::abs(value - expected) < 0.02f;
}

bool isRed(const Color& color)
{
    return roughly(color.r, 1.f) && roughly(color.g, 0.f) && roughly(color.b, 0.f)
           && roughly(color.a, 1.f);
}

bool isTransparent(const Color& color)
{
    return roughly(color.a, 0.f);
}

bool isOpaque(const Color& color)
{
    return roughly(color.a, 1.f);
}

// Real-world icons downloaded verbatim from their upstream repositories;
// see TestFiles/README.md for sources and licenses.
std::string readTestFile(const std::string& name)
{
    auto file = std::ifstream {std::filesystem::path {EACP_SVG_TEST_FILES} / name};
    auto buffer = std::stringstream {};
    buffer << file.rdbuf();
    return buffer.str();
}

Image renderTestFile(const std::string& name)
{
    return eacp::SVG::toImage(readTestFile(name), 128, 128);
}
} // namespace

auto tNaturalSize = test("SVGImage/naturalSize") = []
{
    auto image = eacp::SVG::toImage(redSquareSvg);
    check(image.isValid());
    check(image.width() == 8);
    check(image.height() == 8);
    check(isRed(image.at(4, 4)));
};

auto tExplicitSizeStretches = test("SVGImage/explicitSizeStretches") = []
{
    auto image = eacp::SVG::toImage(redSquareSvg, 16, 4);
    check(image.width() == 16);
    check(image.height() == 4);
    check(isRed(image.at(8, 2)));
};

auto tSingleDimensionKeepsAspect = test("SVGImage/singleDimensionKeepsAspect") = []
{
    auto image = eacp::SVG::toImage(redSquareSvg, 32);
    check(image.width() == 32);
    check(image.height() == 32);
};

auto tUncoveredPixelsAreTransparent =
    test("SVGImage/uncoveredPixelsAreTransparent") = []
{
    constexpr auto svg = R"(<svg width="8" height="8">
      <rect x="4" y="0" width="4" height="8" fill="#ff0000"/>
    </svg>)";

    auto image = eacp::SVG::toImage(svg);
    check(isTransparent(image.at(1, 4)));
    check(isRed(image.at(6, 4)));
};

auto tCircleCoversCenterNotCorner = test("SVGImage/circleCoversCenterNotCorner") = []
{
    constexpr auto svg = R"(<svg width="16" height="16">
      <circle cx="8" cy="8" r="6" fill="#ff0000"/>
    </svg>)";

    auto image = eacp::SVG::toImage(svg);
    check(isRed(image.at(8, 8)));
    check(isTransparent(image.at(0, 0)));
};

auto tGroupTranslateMovesContent = test("SVGImage/groupTranslateMovesContent") = []
{
    constexpr auto svg = R"SVG(<svg width="8" height="8">
      <g transform="translate(4, 0)">
        <rect x="0" y="0" width="4" height="8" fill="#ff0000"/>
      </g>
    </svg>)SVG";

    auto image = eacp::SVG::toImage(svg);
    check(isTransparent(image.at(1, 4)));
    check(isRed(image.at(6, 4)));
};

auto tInvalidMarkupReturnsInvalid = test("SVGImage/invalidMarkupReturnsInvalid") = []
{
    check(!eacp::SVG::toImage("not svg at all"));
    check(!eacp::SVG::toImage("<div>nope</div>"));
};

auto tOpacityAppliesToFill = test("SVGImage/opacityAppliesToFill") = []
{
    constexpr auto svg = R"(<svg width="4" height="4">
      <rect x="0" y="0" width="4" height="4" fill="#ff0000" opacity="0.5"/>
    </svg>)";

    auto image = eacp::SVG::toImage(svg);
    auto center = image.at(2, 2);
    check(roughly(center.a, 0.5f));
};

// Stroke-only logo art: the paths carry no fill of their own and rely on
// inheriting fill="none" from the root <svg>. A renderer without attribute
// inheritance would fill each open path black.
constexpr auto logoSvg =
    R"SVG(<svg width="585" height="510" viewBox="0 0 585 510" fill="none" xmlns="http://www.w3.org/2000/svg">
<path d="M193.823 193.365V27H27V193.365" stroke="black" stroke-width="54"/>
<path d="M376.159 509.48V426.527C376.159 380.46 338.814 343.115 292.747 343.115C246.681 343.115 209.336 380.46 209.336 426.527V509.48" stroke="black" stroke-width="54"/>
<path d="M558 193.365V27H391.177V193.365" stroke="black" stroke-width="54"/>
<path d="M197.445 13.622L292.413 182.393L387.709 13.622" stroke="black" stroke-width="54" stroke-linejoin="bevel"/>
<path d="M193.723 192.984C193.723 247.439 237.979 291.584 292.571 291.584C347.163 291.584 391.177 247.687 391.177 193.231" stroke="black" stroke-width="54"/>
</svg>
)SVG";

auto tLogoInheritsFillNone = test("SVGImage/logoInheritsFillNone") = []
{
    auto image = eacp::SVG::toImage(logoSvg);
    check(image.isValid());
    check(image.width() == 585);
    check(image.height() == 510);

    // Inside the left bracket shape: enclosed by the path but unfilled,
    // so it must stay transparent.
    check(isTransparent(image.at(110, 110)));

    // On the bracket's left vertical stroke (centred on x=27, width 54).
    auto stroke = image.at(27, 100);
    check(roughly(stroke.a, 1.f));
    check(roughly(stroke.r, 0.f));
};

auto tLogoStrokeWidthScalesDown = test("SVGImage/logoStrokeWidthScalesDown") = []
{
    auto image = eacp::SVG::toImage(logoSvg, 36, 36);
    check(image.width() == 36);
    check(image.height() == 36);

    // At tray-icon size the 54-unit strokes shrink to ~3px; an unscaled
    // stroke would swallow the open interior at (7, 7).
    check(isTransparent(image.at(7, 7)));
};

// The V path carries stroke-linejoin="bevel": its bottom vertex at
// (292, 182) must be cut flat. A miter join would spike ~55 units past
// the vertex (half stroke width / sin(half angle)) and blacken the space
// above the bowl.
auto tBevelJoinFlattensTheVee = test("SVGImage/bevelJoinFlattensTheVee") = []
{
    auto image = eacp::SVG::toImage(logoSvg);
    check(isTransparent(image.at(292, 235)));
};

// A V without stroke-linejoin keeps the default miter spike.
auto tMiterJoinStaysPointy = test("SVGImage/miterJoinStaysPointy") = []
{
    constexpr auto svg =
        R"SVG(<svg width="585" height="510" fill="none">
      <path d="M197.445 13.622L292.413 182.393L387.709 13.622" stroke="black" stroke-width="54"/>
    </svg>)SVG";

    auto image = eacp::SVG::toImage(svg);
    check(!isTransparent(image.at(292, 235)));
};

// A circle drawn as two half-circle arc commands. Locks the arc-to-cubic
// conversion independently of the downloaded icons: the parser used to
// drop A/a commands entirely (and lose the pen position with them).
auto tArcCommandsDrawCircle = test("SVGImage/arcCommandsDrawCircle") = []
{
    constexpr auto svg = R"SVG(<svg width="16" height="16">
      <path d="M2 8A6 6 0 1 1 14 8A6 6 0 1 1 2 8" fill="#ff0000"/>
    </svg>)SVG";

    auto image = eacp::SVG::toImage(svg);
    check(isRed(image.at(8, 8)));
    check(isTransparent(image.at(1, 1)));
    check(isTransparent(image.at(15, 15)));
};

// The arc grammar allows the two flags to run straight into the next
// number ("0 1112 0" = flags 1,1 then x=12), so they cannot be read as
// floats. Both spellings must produce the identical image.
auto tArcFlagsWithoutSeparators = test("SVGImage/arcFlagsWithoutSeparators") = []
{
    constexpr auto separated = R"SVG(<svg width="16" height="16">
      <path d="M2 8a6 6 0 1 1 12 0a6 6 0 1 1 -12 0" fill="#ff0000"/>
    </svg>)SVG";
    constexpr auto compact = R"SVG(<svg width="16" height="16">
      <path d="M2 8a6 6 0 1112 0a6 6 0 11-12 0" fill="#ff0000"/>
    </svg>)SVG";

    auto reference = eacp::SVG::toImage(separated);
    check(isRed(reference.at(8, 8)));
    check(eacp::SVG::toImage(compact) == reference);
};

// An <svg> with no width/height sizes itself from the viewBox, not the
// 300x150 CSS fallback.
auto tViewBoxOnlySizing = test("SVGImage/viewBoxOnlySizing") = []
{
    auto image = eacp::SVG::toImage(readTestFile("simpleicons-github.svg"));
    check(image.width() == 24);
    check(image.height() == 24);
};

auto tGithubMark = test("SVGImage/realWorld/githubMark") = []
{
    auto image = renderTestFile("simpleicons-github.svg");
    check(isOpaque(image.at(64, 12)));
    check(isTransparent(image.at(64, 64)));
    check(isTransparent(image.at(4, 4)));
};

auto tBootstrapCamera = test("SVGImage/realWorld/bootstrapCamera") = []
{
    auto image = renderTestFile("bootstrap-camera.svg");
    check(isOpaque(image.at(40, 76)));
    check(isTransparent(image.at(64, 32)));
    check(isTransparent(image.at(4, 4)));
};

auto tBootstrapHeartFill = test("SVGImage/realWorld/bootstrapHeartFill") = []
{
    auto image = renderTestFile("bootstrap-heart-fill.svg");
    check(isOpaque(image.at(64, 64)));
    check(isTransparent(image.at(64, 4)));
};

auto tFeatherActivity = test("SVGImage/realWorld/featherActivity") = []
{
    auto image = renderTestFile("feather-activity.svg");
    check(isOpaque(image.at(64, 64)));
    check(isOpaque(image.at(12, 64)));
    check(isTransparent(image.at(4, 4)));
};

auto tFeatherCheckCircle = test("SVGImage/realWorld/featherCheckCircle") = []
{
    auto image = renderTestFile("feather-check-circle.svg");
    check(isOpaque(image.at(64, 12)));
    check(isTransparent(image.at(64, 64)));
};

auto tMaterialHome = test("SVGImage/realWorld/materialHome") = []
{
    auto image = renderTestFile("material-home.svg");
    check(isOpaque(image.at(64, 64)));
    check(isOpaque(image.at(64, 24)));
    check(isTransparent(image.at(4, 4)));
};

auto tTablerStar = test("SVGImage/realWorld/tablerStar") = []
{
    auto image = renderTestFile("tabler-star.svg");
    check(isOpaque(image.at(64, 12)));
    check(isTransparent(image.at(64, 64)));
    check(isTransparent(image.at(4, 4)));
};

// Writes every render into the temp dir so a test run leaves the images
// ready for inspection by eye.
auto tRendersToDisk = test("SVGImage/rendersToDisk") = []
{
    auto dir = std::filesystem::temp_directory_path();

    auto logo = eacp::SVG::toImage(logoSvg);
    check(logo.isValid());
    logo.save(dir / "eacp-svg-logo.png");

    for (auto& entry: std::filesystem::directory_iterator {EACP_SVG_TEST_FILES})
    {
        if (entry.path().extension() != ".svg")
            continue;

        auto image = renderTestFile(entry.path().filename().string());
        check(image.isValid());
        image.save(dir / entry.path().filename().replace_extension(".png"));
    }
};
