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

auto tStretch = test("CameraView/computeImageAreaStretch") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Stretch);
    check(rectEquals(rect, 0, 0, 100, 100));
};

auto tContainWide = test("CameraView/computeImageAreaContainWide") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Contain);
    check(rectEquals(rect, 0, 25, 100, 50));
};

auto tCoverWide = test("CameraView/computeImageAreaCoverWide") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 200, 100, CameraView::Fit::Cover);
    check(rectEquals(rect, -50, 0, 200, 100));
};

auto tContainTall = test("CameraView/computeImageAreaContainTall") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 100, 200, CameraView::Fit::Contain);
    check(rectEquals(rect, 25, 0, 50, 100));
};

auto tCoverTall = test("CameraView/computeImageAreaCoverTall") = []
{
    auto rect =
        CameraView::computeImageArea(100, 100, 100, 200, CameraView::Fit::Cover);
    check(rectEquals(rect, 0, -50, 100, 200));
};

auto tDegenerate = test("CameraView/computeImageAreaDegenerate") = []
{
    auto rect = CameraView::computeImageArea(100, 100, 0, 0, CameraView::Fit::Cover);
    check(rectEquals(rect, 0, 0, 100, 100));
};
