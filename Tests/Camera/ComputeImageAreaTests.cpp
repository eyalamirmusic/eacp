#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::Cameras;

namespace
{
bool approx(float a, float b)
{
    return std::abs(a - b) < 0.001f;
}

bool rectEquals(const Graphics::Rect& rect, float x, float y, float w, float h)
{
    return approx(rect.x, x) && approx(rect.y, y) && approx(rect.w, w)
           && approx(rect.h, h);
}
} // namespace

// Stretch ignores aspect and fills the whole view.
auto tStretch = test("CameraView/computeImageAreaStretch") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Stretch);
    check(rectEquals(rect, 0, 0, 100, 100));
};

// A 2:1 image in a 1:1 view: Contain fills width and letterboxes top/bottom.
auto tContainWide = test("CameraView/computeImageAreaContainWide") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Contain);
    check(rectEquals(rect, 0, 25, 100, 50));
};

// Same image, Cover: fills height and crops the overflow left/right.
auto tCoverWide = test("CameraView/computeImageAreaCoverWide") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Cover);
    check(rectEquals(rect, -50, 0, 200, 100));
};

// A 1:2 image in a 1:1 view: Contain fills height and letterboxes left/right.
auto tContainTall = test("CameraView/computeImageAreaContainTall") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 100, 200, CameraView::Fit::Contain);
    check(rectEquals(rect, 25, 0, 50, 100));
};

// Same image, Cover: fills width and crops top/bottom.
auto tCoverTall = test("CameraView/computeImageAreaCoverTall") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 100, 200, CameraView::Fit::Cover);
    check(rectEquals(rect, 0, -50, 100, 200));
};

// A degenerate (no-pixels) frame fills the whole view regardless of fit.
auto tDegenerate = test("CameraView/computeImageAreaDegenerate") = []
{
    auto rect = CameraView::computeImageArea(100, 100, 0, 0, CameraView::Fit::Cover);
    check(rectEquals(rect, 0, 0, 100, 100));
};
