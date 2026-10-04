#include <eacp/GPU/GPU.h>
#include <eacp/Text/Text.h>

#include <algorithm>

using namespace eacp;
using namespace GPU;
using namespace Maths;

struct Vertex
{
    Vec2 position;
    Graphics::Color color;
};

EACP_SHADER_VALUE(Graphics::Color, Float4)

namespace
{
const Array<Vertex, 3> triangleVertices = {
    Vertex {{0.0f, 0.5f}, {1.0f, 0.2f, 0.2f}},
    Vertex {{-0.5f, -0.3f}, {0.2f, 1.0f, 0.2f}},
    Vertex {{0.5f, -0.3f}, {0.2f, 0.2f, 1.0f}},
};

struct TriangleShader final : ShaderProgram
{
    TriangleShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto color = vertexInput(&Vertex::color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, (px * s + py * c) * aspect);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(varying(color));
    }

    Uniform<Float> angle;
    Uniform<Float> aspect;

    EACP_SHADER(angle, aspect)
};

bool isControlCharacter(char character)
{
    return static_cast<unsigned char>(character) < 0x20;
}

const char* phaseName(Graphics::TouchPhase phase)
{
    switch (phase)
    {
        case Graphics::TouchPhase::Began:
            return "began";
        case Graphics::TouchPhase::Moved:
            return "moved";
        case Graphics::TouchPhase::Ended:
            return "ended";
        case Graphics::TouchPhase::Cancelled:
            return "cancelled";
    }

    return "";
}

void logTouch(const Graphics::TouchEvent& touch)
{
    LOG("touch ",
        touch.id,
        " ",
        phaseName(touch.phase),
        " at ",
        touch.pos.x,
        ", ",
        touch.pos.y);
}
} // namespace

struct HelloView final : GPUView
{
    HelloView()
    {
        setHandlesTouchEvents(true);
        setWantsTextInput(true);
        setContinuous(true);

        triangle.setVertices(triangleVertices);
        triangle.prepare(sampleCount());
        text.setSampleCount(sampleCount());
    }

    void touchBegan(const Graphics::TouchEvent& touch) override
    {
        logTouch(touch);
        follow(touch.pos);
    }

    void touchMoved(const Graphics::TouchEvent& touch) override
    {
        follow(touch.pos);
    }
    void touchEnded(const Graphics::TouchEvent& touch) override
    {
        logTouch(touch);
        focus();
    }

    void keyDown(const Graphics::KeyEvent& key) override
    {
        if (key.keyCode == Graphics::KeyCode::Delete)
        {
            if (!typed.empty())
                typed.pop_back();

            return;
        }

        if (key.characters.empty() || isControlCharacter(key.characters.front()))
        {
            passKeyOn();
            return;
        }

        typed += key.characters;
    }

    void follow(Graphics::Point position)
    {
        auto bounds = getLocalBounds();
        red = std::clamp(position.x / std::max(bounds.w, 1.f), 0.f, 1.f);
        blue = std::clamp(position.y / std::max(bounds.h, 1.f), 0.f, 1.f);
    }

    void update(Threads::FrameTime time) override
    {
        angle += radiansPerSecond * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        auto size = frame.logicalSize();
        auto pass = frame.beginPass({Graphics::Color {red, 0.45f, blue}});

        triangle.angle = angle;
        triangle.aspect = size.x / std::max(size.y, 1.f);
        pass.draw(triangle);

        drawText(pass, frame);
    }

    void drawText(RenderPass& pass, Frame& frame)
    {
        auto size = frame.logicalSize();
        auto title = Text::Font {"sans-serif", 28.f, Text::FontStyle::Bold};
        auto caption = Text::Font {"monospace", 15.f};
        auto shadow = Graphics::Color {0.f, 0.f, 0.f, 0.35f};
        auto white = Graphics::Color::white();

        text.setViewport(size, frame.backingScale());
        text.begin();

        auto centred = [&](std::string_view line, float y, const Text::Font& font)
        {
            auto x = (size.x - text.measure(line, font)) * 0.5f;
            text.draw(line, {x + 1.f, y + 1.f}, shadow, font);
            text.draw(line, {x, y}, white, font);
        };

        centred("Hello from eacp", size.y * 0.18f, title);
        centred("Vulkan + android.graphics text", size.y * 0.18f + 30.f, caption);
        centred(typed.empty() ? "touch to change the colour and type" : typed,
                std::min(size.y * 0.85f, size.y - getSafeAreaInsets().bottom - 30.f),
                caption);

        text.flush(pass);
    }

    static constexpr float radiansPerSecond = 1.2f;

    TriangleShader triangle;
    Text::TextRenderer text;

    std::string typed;
    float angle = 0.f;
    float red = 0.95f;
    float blue = 0.35f;
};

int main()
{
    LOG("eacp HelloGPU: ",
        Device::shared().isValid() ? "Vulkan device up" : "no device");
    Apps::setSuspendHandler([](bool suspended)
                            { LOG(suspended ? "suspended" : "resumed"); });
    return Graphics::runWindowedApp<HelloView>();
}
