#include <eacp/GPUWidgets/GPUWidgets.h>

using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
constexpr float pi = 3.14159265358979323846f;

// The paths below are authored in a fixed 800 x 520 design space (see
// setCoordinateSpace), so they keep filling the window whatever its size.
constexpr float designWidth = 800.0f;
constexpr float designHeight = 520.0f;

// A concave star: a moveTo / lineTo polygon that only fills correctly once the
// tessellator handles reflex corners, so it exercises the ear clipper.
void addStar(
    Path& path, float centerX, float centerY, float outerRadius, float innerRadius)
{
    auto pointCount = 5;

    for (auto i = 0; i < pointCount * 2; ++i)
    {
        auto radius = (i % 2 == 0) ? outerRadius : innerRadius;
        auto angle = -pi * 0.5f + (float) i * pi / (float) pointCount;
        auto point = Graphics::Point {centerX + std::cos(angle) * radius,
                                      centerY + std::sin(angle) * radius};

        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }

    path.close();
}

// A smooth organic blob built from four cubic Beziers, exercising curve
// flattening.
void addBlob(Path& path, float centerX, float centerY, float radius)
{
    path.moveTo({centerX + radius, centerY});
    path.cubicTo(centerX + radius,
                 centerY - radius * 0.9f,
                 centerX + radius * 0.4f,
                 centerY - radius,
                 centerX,
                 centerY - radius);
    path.cubicTo(centerX - radius * 0.4f,
                 centerY - radius,
                 centerX - radius,
                 centerY - radius * 0.6f,
                 centerX - radius,
                 centerY);
    path.cubicTo(centerX - radius,
                 centerY + radius * 0.6f,
                 centerX - radius * 0.5f,
                 centerY + radius,
                 centerX,
                 centerY + radius);
    path.cubicTo(centerX + radius * 0.5f,
                 centerY + radius,
                 centerX + radius,
                 centerY + radius * 0.9f,
                 centerX + radius,
                 centerY);
    path.close();
}

Path buildScene()
{
    auto scene = Path {};

    scene.addRoundedRect({60.0f, 50.0f, 280.0f, 170.0f}, 34.0f);
    scene.addEllipse({460.0f, 50.0f, 280.0f, 170.0f});
    addStar(scene, 200.0f, 380.0f, 110.0f, 46.0f);
    addBlob(scene, 600.0f, 380.0f, 110.0f);

    return scene;
}
} // namespace

struct PathsApp
{
    PathsApp()
    {
        // A vertical gradient across the whole design space, so shapes higher in
        // the window read warm and lower ones cool.
        auto gradient = Graphics::LinearGradient {
            {0.0f, 0.0f},
            {0.0f, designHeight},
            {{Graphics::Color {0.98f, 0.62f, 0.20f}, 0.0f},
             {Graphics::Color {0.92f, 0.24f, 0.48f}, 0.5f},
             {Graphics::Color {0.36f, 0.24f, 0.72f}, 1.0f}}};

        view.setCoordinateSpace(designWidth, designHeight);
        view.setBackgroundColor({0.07f, 0.08f, 0.11f, 1.0f});
        view.setFillGradient(gradient);
        view.setStrokeColor({0.96f, 0.97f, 1.0f, 1.0f});
        view.setStrokeWidth(6.0f);
        view.setPath(buildScene());

        window.setContentView(view);
    }

    PathView view;
    Graphics::Window window {[]
                             {
                                 auto options = Graphics::WindowOptions {};
                                 options.width = (int) designWidth;
                                 options.height = (int) designHeight;
                                 options.title = "GPUWidgets - Paths";
                                 return options;
                             }()};
};

int main()
{
    eacp::Apps::run<PathsApp>();
    return 0;
}
