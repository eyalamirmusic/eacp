#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace eacp;
using namespace GPU;

// Three windows, one per test surface. All three use the shared shader math
// authored via emitShaderBody() so a break in any one panel isolates to that
// panel's plumbing (shader / drawInstanced / firstInstance) rather than
// something upstream.

namespace
{
constexpr int windowW = 600;
constexpr int windowH = 750;
constexpr int windowGap = 20;

// The GPU render's Y range in NDC. Labels sit above and below this band; the
// GPU view fills the whole window and the labels overlay as a sibling paint
// surface, so keeping the geometry within [gpuBotY, gpuTopY] keeps triangles
// clear of the label text.
constexpr float gpuTopY = +0.72f;
constexpr float gpuBotY = -0.72f;

// Non-instanced grid (window 1).
constexpr int nonInstCols = 3;
constexpr int nonInstRows = 12;
constexpr int nonInstCount = nonInstCols * nonInstRows; // 36
constexpr float nonInstTriRadius = 0.055f;

// Instanced grid (windows 2 and 3 share the same 40x25 layout).
constexpr int gridCols = 40;
constexpr int gridRows = 25;
constexpr int instanceCount = gridCols * gridRows; // 1000
constexpr float instTriRadius = 0.020f;
constexpr float rowScanTriRadius = 0.028f;

constexpr float gridLeftX = -0.9f;
constexpr float gridRightX = +0.9f;

// Row-scan cadence for window 3: each row of 40 stays visible for rowSeconds,
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

struct Uniforms
{
    float time;
    float scale;
    float _pad0 = 0.f;
    float _pad1 = 0.f;
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

// Same shader body, three different input sources across the three pipelines.
struct Inputs
{
    Float2 position;
    Float2 uv;
    Float2 center;
    Float rotationSpeed;
    Float3 color;
    Float time;
    Float scale;
};

void emitShaderBody(ShaderBuilder& builder, const Inputs& in)
{
    auto varyingUv = builder.varying(in.uv);
    auto varyingColor = builder.varying(in.color);

    auto angle = in.time * in.rotationSpeed;
    auto c = cos(angle);
    auto s = sin(angle);

    auto px = in.position.x();
    auto py = in.position.y();
    auto rotatedX = (px * c - py * s) * in.scale;
    auto rotatedY = (px * s + py * c) * in.scale;

    auto worldX = in.center.x() + rotatedX;
    auto worldY = in.center.y() + rotatedY;

    builder.position(float4(worldX, worldY, 0.f, 1.f));

    auto brightness = varyingUv.y() * 0.7f + 0.3f;
    auto shaded = varyingColor * brightness;
    builder.fragment(float4(shaded, 1.f));
}

GeneratedShader makeInstancedShader()
{
    auto builder = ShaderBuilder {};
    Inputs in;
    in.position = builder.vertexInput<Float2>();
    in.uv = builder.vertexInput<Float2>();
    in.center = builder.instanceInput<Float2>();
    in.rotationSpeed = builder.instanceInput<Float>();
    in.color = builder.instanceInput<Float3>(2);
    in.time = builder.uniform<Float>();
    in.scale = builder.uniform<Float>();
    emitShaderBody(builder, in);
    return builder.build();
}

GeneratedShader makeNonInstancedShader()
{
    auto builder = ShaderBuilder {};
    Inputs in;
    in.position = builder.vertexInput<Float2>();
    in.uv = builder.vertexInput<Float2>();
    in.center = builder.vertexInput<Float2>();
    in.rotationSpeed = builder.vertexInput<Float>();
    in.color = builder.vertexInput<Float3>();
    in.time = builder.uniform<Float>();
    in.scale = builder.uniform<Float>();
    emitShaderBody(builder, in);
    return builder.build();
}

Graphics::WindowOptions windowOptions(std::string title, int slotIndex)
{
    auto options = Graphics::WindowOptions {};
    options.width = windowW;
    options.height = windowH;
    options.title = std::move(title);
    // Place the three windows side by side at the top of the primary display,
    // slightly offset from the corner so the title bars are visible.
    options.initialPosition =
        Graphics::Point {(float) (40 + slotIndex * (windowW + windowGap)), 80.f};
    return options;
}
} // namespace

// ─── Views ──────────────────────────────────────────────────────────────

struct NonInstancedView final : GPUView
{
    NonInstancedView()
        : shader(makeNonInstancedShader())
        , fatVertsData(buildNonInstancedVerts())
        , fatVertsBuffer(Device::shared().makeBuffer(
              fatVertsData.data(), fatVertsData.size() * sizeof(FatVertex)))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
    {
        setContinuous(true);
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        auto uniforms = Uniforms {elapsed, nonInstTriRadius};
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(fatVertsBuffer, 0);
        pass.setVertexBytes(&uniforms, sizeof(uniforms), 0);
        pass.draw((int) fatVertsData.size());
    }

    float elapsed = 0.f;
    GeneratedShader shader;
    std::vector<FatVertex> fatVertsData;
    Buffer fatVertsBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
};

struct InstancedView final : GPUView
{
    InstancedView()
        : shader(makeInstancedShader())
        , unitTriangleBuffer(Device::shared().makeBuffer(unitTriangleVerts))
        , transformsData(buildInstancedTransforms())
        , transformsBuffer(Device::shared().makeBuffer(
              transformsData.data(),
              transformsData.size() * sizeof(PerInstanceTransform)))
        , colorsData(buildInstancedColors())
        , colorsBuffer(Device::shared().makeBuffer(
              colorsData.data(), colorsData.size() * sizeof(PerInstanceColor)))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
    {
        setContinuous(true);
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        auto uniforms = Uniforms {elapsed, instTriRadius};
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(unitTriangleBuffer, 0);
        pass.setVertexBuffer(transformsBuffer, 1);
        pass.setVertexBuffer(colorsBuffer, 2);
        pass.setVertexBytes(&uniforms, sizeof(uniforms), 0);
        pass.drawInstanced(3, instanceCount);
    }

    float elapsed = 0.f;
    GeneratedShader shader;
    Buffer unitTriangleBuffer;
    std::vector<PerInstanceTransform> transformsData;
    Buffer transformsBuffer;
    std::vector<PerInstanceColor> colorsData;
    Buffer colorsBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
};

// Same buffers and pipeline shape as InstancedView, but draws only ONE row's
// worth (gridCols instances) each frame, walking `firstInstance` down the
// buffer to make a single horizontal strip of triangles scan through the
// panel top-to-bottom.
struct RowScanView final : GPUView
{
    static constexpr std::uint16_t indices[3] = {0, 1, 2};

    RowScanView()
        : shader(makeInstancedShader())
        , unitTriangleBuffer(Device::shared().makeBuffer(unitTriangleVerts))
        , transformsData(buildInstancedTransforms())
        , transformsBuffer(Device::shared().makeBuffer(
              transformsData.data(),
              transformsData.size() * sizeof(PerInstanceTransform)))
        , colorsData(buildInstancedColors())
        , colorsBuffer(Device::shared().makeBuffer(
              colorsData.data(), colorsData.size() * sizeof(PerInstanceColor)))
        , indexBuffer(Device::shared().makeBuffer(indices, BufferUsage::Index))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
    {
        setContinuous(true);
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void update(Threads::FrameTime time) override { elapsed += (float) time.delta; }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});

        // Row 0 sits at the bottom of the grid; scan visually top-to-bottom
        // by inverting the row index.
        auto stepIndex = (int) (elapsed / rowScanRowSeconds);
        auto rowFromBottom = (gridRows - 1) - (stepIndex % gridRows);
        auto firstInstance = rowFromBottom * gridCols;

        auto uniforms = Uniforms {elapsed, rowScanTriRadius};
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(unitTriangleBuffer, 0);
        pass.setVertexBuffer(transformsBuffer, 1);
        pass.setVertexBuffer(colorsBuffer, 2);
        pass.setVertexBytes(&uniforms, sizeof(uniforms), 0);
        pass.drawIndexedInstanced(
            indexBuffer, 3, gridCols, IndexFormat::UInt16, 0, firstInstance);
    }

    float elapsed = 0.f;
    GeneratedShader shader;
    Buffer unitTriangleBuffer;
    std::vector<PerInstanceTransform> transformsData;
    Buffer transformsBuffer;
    std::vector<PerInstanceColor> colorsData;
    Buffer colorsBuffer;
    Buffer indexBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
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

        g.setColor(Graphics::Color::white());

        g.drawText(
            title, {cx - (float) title.size() * halfCharTitle, titleY}, titleFont);
        g.drawText(youShouldSee,
                   {cx - (float) youShouldSee.size() * halfCharBody,
                    bounds.h - bodyLine1Yoff},
                   bodyFont);
        g.drawText(
            thisTests,
            {cx - (float) thisTests.size() * halfCharBody, bounds.h - bodyLine2Yoff},
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

struct RootView final : Graphics::View
{
    void resized() override
    {
        for (auto* child: getSubviews())
            child->setBounds(getLocalBounds());
    }
};

// ─── App: three parallel windows ────────────────────────────────────────

struct InstancingApp
{
    InstancingApp()
    {
        rootA.addSubview(gpuA);
        rootA.addSubview(labelA);
        windowA.setContentView(rootA);

        rootB.addSubview(gpuB);
        rootB.addSubview(labelB);
        windowB.setContentView(rootB);

        rootC.addSubview(gpuC);
        rootC.addSubview(labelC);
        windowC.setContentView(rootC);
    }

    // Window A - baseline (no instancing).
    RootView rootA;
    NonInstancedView gpuA;
    LabelView labelA {
        "Non-instanced",
        "you should see: 36 triangles, hue by row, spinning at varied rates",
        "tests: the shared shader math without instancing (baseline)",
    };
    Graphics::Window windowA {windowOptions("eacp - Non-instanced", 0)};

    // Window B - drawInstanced.
    RootView rootB;
    InstancedView gpuB;
    LabelView labelB {
        "drawInstanced",
        "you should see: 1000 triangles (40x25), rainbow rows, all spinning at varied rates",
        "tests: multi-buffer layout (slots 0/1/2) and drawInstanced(3, 1000)",
    };
    Graphics::Window windowB {windowOptions("eacp - drawInstanced", 1)};

    // Window C - drawIndexedInstanced + firstInstance.
    RootView rootC;
    RowScanView gpuC;
    LabelView labelC {
        "drawIndexedInstanced + firstInstance",
        "you should see: one row of 40 triangles at a time scanning top->bottom over time",
        "tests: firstInstance offset stepping through the buffer, indexed instanced draw",
    };
    Graphics::Window windowC {windowOptions("eacp - firstInstance scan", 2)};
};

int main()
{
    eacp::Apps::run<InstancingApp>();
    return 0;
}
