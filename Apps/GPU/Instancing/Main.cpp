#include <eacp/GPU/GPU.h>

#include <vector>

using namespace eacp;
using namespace GPU;

// One window, three side-by-side panels - each a GPUView with its own render
// pass, so a break in any one panel isolates to that panel's plumbing (shader /
// drawInstanced / firstInstance) rather than something upstream.
//
// All three panels are authored as ShaderPrograms sharing one shader body
// (SpinProgram::emitBody). The panels differ only in where the per-triangle
// center / speed / colour come from: pulled per-vertex for the non-instanced
// baseline, or per-instance for the two instanced panels. The body never knows
// the difference.

namespace
{
// Per-panel width the window opens at (three panels side by side); the layout
// re-flows to equal thirds on resize, so this is only the initial size.
constexpr int windowW = 480;
constexpr int windowH = 750;

// The GPU render's Y range in NDC. Labels sit above and below this band; the
// GPU view fills its panel and the labels overlay as a sibling paint surface,
// so keeping the geometry within [gpuBotY, gpuTopY] keeps triangles clear of
// the label text.
constexpr float gpuTopY = +0.72f;
constexpr float gpuBotY = -0.72f;

// Non-instanced grid (panel 1).
constexpr int nonInstCols = 3;
constexpr int nonInstRows = 12;
constexpr int nonInstCount = nonInstCols * nonInstRows; // 36
constexpr float nonInstTriRadius = 0.055f;

// Instanced grid (panels 2 and 3 share the same 40x25 layout).
constexpr int gridCols = 40;
constexpr int gridRows = 25;
constexpr int instanceCount = gridCols * gridRows; // 1000
constexpr float instTriRadius = 0.020f;
constexpr float rowScanTriRadius = 0.028f;

constexpr float gridLeftX = -0.9f;
constexpr float gridRightX = +0.9f;

// Row-scan cadence for panel 3: each row of 40 stays visible for rowSeconds,
// so a full top-to-bottom scan takes ~gridRows * rowSeconds seconds.
constexpr float rowScanRowSeconds = 0.2f;

// Slot 0 (both instanced pipelines): unit triangle corner + UV. Same 3 verts
// drive every triangle; the non-instanced pipeline expands these into 3 verts
// per triangle in its fat vertex buffer.
struct PerVertex
{
    float position[2];
    float uv[2];
};

// Slot 1 in the instanced pipeline: placement + spin rate per instance.
struct PerInstanceTransform
{
    float center[2];
    float rotationSpeed;
};

// Slot 2 in the instanced pipeline: colour per instance.
struct PerInstanceColor
{
    float color[3];
};

// The non-instanced pipeline's one and only vertex stream. Each corner of
// each triangle carries the full set - centre / speed / colour values are
// duplicated three times per triangle. Exactly what instancing eliminates.
struct FatVertex
{
    float position[2];
    float uv[2];
    float center[2];
    float rotationSpeed;
    float color[3];
};

constexpr PerVertex unitTriangleVerts[] = {
    {{0.f, +1.f}, {0.5f, 1.f}},
    {{-0.8660254f, -0.5f}, {0.f, 0.f}},
    {{+0.8660254f, -0.5f}, {1.f, 0.f}},
};

// HSV -> RGB with S = V = 1, so hue picks a pure spectral colour.
Graphics::Color hueColor(float hueTurns)
{
    auto wrap = hueTurns - std::floor(hueTurns);
    auto h6 = wrap * 6.f;
    auto x = 1.f - std::fabs(std::fmod(h6, 2.f) - 1.f);

    float r = 0.f, g = 0.f, b = 0.f;

    if (h6 < 1.f)
    {
        r = 1.f;
        g = x;
        b = 0.f;
    }
    else if (h6 < 2.f)
    {
        r = x;
        g = 1.f;
        b = 0.f;
    }
    else if (h6 < 3.f)
    {
        r = 0.f;
        g = 1.f;
        b = x;
    }
    else if (h6 < 4.f)
    {
        r = 0.f;
        g = x;
        b = 1.f;
    }
    else if (h6 < 5.f)
    {
        r = x;
        g = 0.f;
        b = 1.f;
    }
    else
    {
        r = 1.f;
        g = 0.f;
        b = x;
    }

    return {r, g, b};
}

// Grid cell centre for a `cols x rows` layout inside `[leftX, rightX] x [botY,
// topY]`. `index` is row-major.
void cellCentre(int index,
                int cols,
                int rows,
                float leftX,
                float rightX,
                float botY,
                float topY,
                float& outX,
                float& outY)
{
    auto row = index / cols;
    auto col = index % cols;
    outX = leftX + (rightX - leftX) * ((float) col + 0.5f) / (float) cols;
    outY = botY + (topY - botY) * ((float) row + 0.5f) / (float) rows;
}

float rotationSpeedFor(int index, int total)
{
    auto normalised = (float) index / (float) (total - 1);
    return 0.5f + 2.0f * normalised;
}

Graphics::Color colorFor(int index, int cols, int rows)
{
    auto row = index / cols;
    return hueColor((float) row / (float) rows);
}

std::vector<PerInstanceTransform> buildInstancedTransforms()
{
    auto out = std::vector<PerInstanceTransform> {};
    out.reserve(instanceCount);
    for (auto i = 0; i < instanceCount; ++i)
    {
        float x, y;
        cellCentre(
            i, gridCols, gridRows, gridLeftX, gridRightX, gpuBotY, gpuTopY, x, y);
        out.push_back({{x, y}, rotationSpeedFor(i, instanceCount)});
    }
    return out;
}

std::vector<PerInstanceColor> buildInstancedColors()
{
    auto out = std::vector<PerInstanceColor> {};
    out.reserve(instanceCount);
    for (auto i = 0; i < instanceCount; ++i)
    {
        auto c = colorFor(i, gridCols, gridRows);
        out.push_back({{c.r, c.g, c.b}});
    }
    return out;
}

// Non-instanced expansion: 3 fat verts per triangle, each carrying the
// triangle's centre / speed / colour on top of its own corner + uv.
std::vector<FatVertex> buildNonInstancedVerts()
{
    auto out = std::vector<FatVertex> {};
    out.reserve(nonInstCount * 3);

    for (auto tri = 0; tri < nonInstCount; ++tri)
    {
        float cx, cy;
        cellCentre(
            tri, nonInstCols, nonInstRows, -0.6f, +0.6f, gpuBotY, gpuTopY, cx, cy);
        auto speed = rotationSpeedFor(tri, nonInstCount);
        auto col = colorFor(tri, nonInstCols, nonInstRows);

        for (const auto& v: unitTriangleVerts)
        {
            out.push_back({
                {v.position[0], v.position[1]},
                {v.uv[0], v.uv[1]},
                {cx, cy},
                speed,
                {col.r, col.g, col.b},
            });
        }
    }
    return out;
}

// ─── Shaders: one body, three input sources ─────────────────────────────

// The handles emitBody() reads. Each panel's define() fills them from either a
// per-vertex or a per-instance source, then hands them to the shared body.
struct SpinInputs
{
    Float2 position;
    Float2 uv;
    Float2 center;
    Float rotationSpeed;
    Float3 color;
};

// The shared shader math: spin the unit triangle by time*rotationSpeed, scale
// it, place it at its center, shade it by UV. Authored once here; the three
// panels reuse it verbatim. `time` and `scale` are named uniform members set
// per frame from render().
struct SpinProgram : ShaderProgram
{
    void emitBody(const SpinInputs& in)
    {
        auto varyingUv = varying(in.uv);
        auto varyingColor = varying(in.color);

        auto angle = time * in.rotationSpeed;
        auto c = cos(angle);
        auto s = sin(angle);

        auto px = in.position.x();
        auto py = in.position.y();
        auto rotatedX = (px * c - py * s) * scale;
        auto rotatedY = (px * s + py * c) * scale;

        setPosition(
            float4(in.center.x() + rotatedX, in.center.y() + rotatedY, 0.f, 1.f));

        auto brightness = varyingUv.y() * 0.7f + 0.3f;
        setFragment(float4(varyingColor * brightness, 1.f));
    }

    Uniform<Float> time;
    Uniform<Float> scale;

    EACP_SHADER(time, scale)
};

// Panel 1: every field is per-vertex, pulled straight out of the fat vertex.
struct NonInstancedProgram final : SpinProgram
{
    NonInstancedProgram() { compile(); }

    void define() override
    {
        SpinInputs in;
        in.position = vertexInput(&FatVertex::position);
        in.uv = vertexInput(&FatVertex::uv);
        in.center = vertexInput(&FatVertex::center);
        in.rotationSpeed = vertexInput(&FatVertex::rotationSpeed);
        in.color = vertexInput(&FatVertex::color);
        emitBody(in);
    }
};

// Panels 2 and 3: geometry + UV per-vertex at slot 0; transform per-instance at
// slot 1; colour per-instance at slot 2. Same body, three buffers.
struct InstancedProgram final : SpinProgram
{
    InstancedProgram() { compile(); }

    void define() override
    {
        SpinInputs in;
        in.position = vertexInput(&PerVertex::position);
        in.uv = vertexInput(&PerVertex::uv);
        in.center = instanceInput(&PerInstanceTransform::center, 1);
        in.rotationSpeed = instanceInput(&PerInstanceTransform::rotationSpeed, 1);
        in.color = instanceInput(&PerInstanceColor::color, 2);
        emitBody(in);
    }
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    // Three panels side by side; the layout re-flows to equal thirds on resize.
    options.width = 3 * windowW;
    options.height = windowH;
    options.title = "eacp - Instancing";
    options.minWidth = windowW;
    options.minHeight = 320;
    return options;
}
} // namespace

// ─── Views ──────────────────────────────────────────────────────────────

struct NonInstancedView final : GPUView
{
    NonInstancedView()
        : fatVertsData(buildNonInstancedVerts())
    {
        shader.setVertices(fatVertsData.data(), (int) fatVertsData.size());
        shader.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        shader.time = elapsed;
        shader.scale = nonInstTriRadius;

        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        pass.draw(shader);
    }

    std::vector<FatVertex> fatVertsData;
    NonInstancedProgram shader;
    float elapsed = 0.f;
};

struct InstancedView final : GPUView
{
    InstancedView()
        : transformsData(buildInstancedTransforms())
        , colorsData(buildInstancedColors())
    {
        shader.setVertices(unitTriangleVerts);
        shader.setInstances(1, transformsData.data(), (int) transformsData.size());
        shader.setInstances(2, colorsData.data(), (int) colorsData.size());
        shader.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        shader.time = elapsed;
        shader.scale = instTriRadius;

        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        pass.drawInstanced(shader, instanceCount);
    }

    std::vector<PerInstanceTransform> transformsData;
    std::vector<PerInstanceColor> colorsData;
    InstancedProgram shader;
    float elapsed = 0.f;
};

// Same buffers and program shape as InstancedView, but draws only ONE row's
// worth (gridCols instances) each frame, walking `firstInstance` down the
// buffer so a single horizontal strip of triangles scans through the panel
// top-to-bottom.
struct RowScanView final : GPUView
{
    static constexpr std::uint16_t indices[3] = {0, 1, 2};

    RowScanView()
        : transformsData(buildInstancedTransforms())
        , colorsData(buildInstancedColors())
    {
        shader.setVertices(unitTriangleVerts);
        shader.setInstances(1, transformsData.data(), (int) transformsData.size());
        shader.setInstances(2, colorsData.data(), (int) colorsData.size());
        shader.setIndices(indices);
        shader.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        // Row 0 sits at the bottom of the grid; scan visually top-to-bottom
        // by inverting the row index.
        auto stepIndex = (int) (elapsed / rowScanRowSeconds);
        auto rowFromBottom = (gridRows - 1) - (stepIndex % gridRows);
        auto firstInstance = rowFromBottom * gridCols;

        shader.time = elapsed;
        shader.scale = rowScanTriRadius;

        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        pass.drawInstanced(shader, gridCols, firstInstance);
    }

    std::vector<PerInstanceTransform> transformsData;
    std::vector<PerInstanceColor> colorsData;
    InstancedProgram shader;
    float elapsed = 0.f;
};

// ─── Chrome: title + bottom "what you see / what this tests" ────────────

struct LabelView final : Graphics::View
{
    LabelView(std::string t, std::string yss, std::string tt)
        : title(std::move(t))
        , youShouldSee(std::move(yss))
        , thisTests(std::move(tt))
    {
    }

    void paint(Graphics::Context& g) override
    {
        constexpr float halfCharTitle = 4.6f;
        constexpr float halfCharBody = 3.3f;
        constexpr float titleY = 28.f;
        constexpr float bodyLine1Yoff = 44.f;
        constexpr float bodyLine2Yoff = 22.f;

        const auto bounds = getLocalBounds();
        const auto cx = bounds.w * 0.5f;

        // Centre each line, but never start it left of a small margin, so a
        // line wider than the panel stays readable from its start instead of
        // being clipped off the left edge.
        constexpr float leftMargin = 8.f;
        const auto startX = [&](const std::string& text, float halfChar)
        { return std::max(leftMargin, cx - (float) text.size() * halfChar); };

        g.setColor(Graphics::Color::white());

        g.drawText(title, {startX(title, halfCharTitle), titleY}, titleFont);
        g.drawText(youShouldSee,
                   {startX(youShouldSee, halfCharBody), bounds.h - bodyLine1Yoff},
                   bodyFont);
        g.drawText(thisTests,
                   {startX(thisTests, halfCharBody), bounds.h - bodyLine2Yoff},
                   bodyFont);
    }

    std::string title;
    std::string youShouldSee;
    std::string thisTests;

    Graphics::Font titleFont {
        Graphics::FontOptions().withName("Menlo").withSize(15.f)};
    Graphics::Font bodyFont {
        Graphics::FontOptions().withName("Menlo").withSize(11.f)};
};

// ─── Layout: one window, three panels side by side ──────────────────────

// One panel: the GPU view fills it, the label overlays it. Insertion order is
// z-order, so the label (added second) sits on top of the GPU view.
struct PanelView final : Graphics::View
{
    void resized() override
    {
        for (auto* child: getSubviews())
            child->setBounds(getLocalBounds());
    }
};

// Tiles its panel children left-to-right in equal columns, so the three test
// surfaces share one window and re-flow on resize.
struct RootView final : Graphics::View
{
    void resized() override
    {
        const auto bounds = getLocalBounds();
        const auto count = getSubviews().size();
        if (count <= 0)
            return;

        const auto panelW = bounds.w / (float) count;
        for (auto i = 0; i < count; ++i)
            getSubviews()[i]->setBounds(
                {bounds.x + (float) i * panelW, bounds.y, panelW, bounds.h});
    }
};

// ─── App: one window, three panels ──────────────────────────────────────

struct InstancingApp
{
    InstancingApp()
    {
        panelA.addSubview(gpuA);
        panelA.addSubview(labelA);

        panelB.addSubview(gpuB);
        panelB.addSubview(labelB);

        panelC.addSubview(gpuC);
        panelC.addSubview(labelC);

        root.addSubview(panelA);
        root.addSubview(panelB);
        root.addSubview(panelC);

        window.setContentView(root);
    }

    RootView root;

    // Panel A - baseline (no instancing).
    PanelView panelA;
    NonInstancedView gpuA;
    LabelView labelA {
        "Non-instanced",
        "you should see: 36 triangles, hue by row, spinning at varied rates",
        "tests: the shared shader math without instancing (baseline)",
    };

    // Panel B - drawInstanced.
    PanelView panelB;
    InstancedView gpuB;
    LabelView labelB {
        "drawInstanced",
        "you should see: 1000 triangles (40x25), rainbow rows, all spinning at varied rates",
        "tests: multi-buffer layout (slots 0/1/2) and drawInstanced(program, 1000)",
    };

    // Panel C - drawIndexedInstanced + firstInstance.
    PanelView panelC;
    RowScanView gpuC;
    LabelView labelC {
        "drawIndexedInstanced + firstInstance",
        "you should see: one row of 40 triangles at a time scanning top->bottom over time",
        "tests: firstInstance offset stepping through the buffer, indexed instanced draw",
    };

    Graphics::Window window {windowOptions()};
};

int main()
{
    eacp::Apps::run<InstancingApp>();
    return 0;
}
